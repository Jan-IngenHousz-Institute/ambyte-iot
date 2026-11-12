/* Remote fetch of an AMBIT release into /sdcard/ambit_fw/<ver>/ (see ambit_fw_fetch.h). */
#include "ambit_fw_fetch.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ambit_fw_manifest.h"
#include "mbedtls/sha256.h"
#include "sd_card.h"
#include "sd_diag.h"

/* Verification build only: `evq_hil fault io ambit_ota …` (LAST include). The
 * fetch is the AMBIT image download writer, so it shares the OTA writer's tag. */
#define EVQ_HIL_WRITER SD_DIAG_W_AMBIT_OTA
#include "evq_hil_io_w.h"

#define FETCH_BUF 1024u

static void fault(sd_diag_op_t op, int err)
{
    sd_diag_fault(SD_DIAG_W_AMBIT_OTA, op, err ? err : EIO);
}

/* Remove a file this fetch wrote or found mismatching. Every name in the folder is
 * single-named and never renamed by the firmware, so removing it cannot free a
 * cross-linked chain. A failed remove leaves a file the manifest gate rejects; the
 * next attempt's step 2 removes it. */
static void drop(const char *path)
{
    if (remove(path) != 0 && errno != ENOENT) fault(SD_DIAG_OP_REMOVE, errno);
}

/* ── manifest download into RAM ─────────────────────────────────────────── */

static const char *get_manifest(const ambit_fw_http_t *http, const char *url, char **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    int64_t clen = -1;
    void *h = http->open(http->ctx, url, &clen);
    if (h == NULL) return "manifest_http_failed";
    if (clen > (int64_t)AMBIT_FW_MANIFEST_MAX) {
        http->close(h);
        return "manifest_too_large";
    }
    char *buf = malloc(AMBIT_FW_MANIFEST_MAX + 2);
    if (buf == NULL) {
        http->close(h);
        return "no_mem";
    }
    size_t total = 0;
    const char *why = NULL;
    while (total <= AMBIT_FW_MANIFEST_MAX) {
        int r = http->read(h, (uint8_t *)buf + total, AMBIT_FW_MANIFEST_MAX + 1 - total);
        if (r < 0) { why = "manifest_read_failed"; break; }
        if (r == 0) break;
        total += (size_t)r;
    }
    http->close(h);
    if (why == NULL && total > AMBIT_FW_MANIFEST_MAX) why = "manifest_too_large";
    if (why == NULL && total == 0) why = "manifest_empty";
    if (why != NULL) {
        free(buf);
        return why;
    }
    buf[total] = '\0';
    *out = buf;
    *len = total;
    return NULL;
}

/* ── SD writes (caller holds an io ref) ─────────────────────────────────── */

/* manifest.json: kept when byte-identical, otherwise replaced by an exclusive
 * create (fsync'd, closed, read back byte for byte). */
static const char *write_manifest(const char *dir, const char *json, size_t len)
{
    char *have = NULL;
    size_t have_len = 0;
    esp_err_t e = ambit_fw_manifest_read(dir, &have, &have_len);
    if (e == ESP_OK) {
        bool same = have_len == len && memcmp(have, json, len) == 0;
        free(have);
        if (same) return NULL;
    } else if (e != ESP_ERR_NOT_FOUND && e != ESP_ERR_INVALID_SIZE) {
        return "sd_read_failed";
    }
    char path[192];
    snprintf(path, sizeof path, "%s/%s", dir, AMBIT_FW_MANIFEST_NAME);
    if (e != ESP_ERR_NOT_FOUND) {
        if (remove(path) != 0 && errno != ENOENT) {
            fault(SD_DIAG_OP_REMOVE, errno);
            return "manifest_remove_failed";
        }
    }
    FILE *f = fopen(path, "wx");
    if (f == NULL) {
        fault(SD_DIAG_OP_OPEN, errno);
        sdcard_report_io_error();
        return "manifest_open_failed";
    }
    const char *why = NULL;
    errno = 0;
    if (fwrite(json, 1, len, f) != len) { fault(SD_DIAG_OP_WRITE, errno); why = "manifest_write_failed"; }
    else if (fflush(f) != 0)            { fault(SD_DIAG_OP_FLUSH, errno); why = "manifest_write_failed"; }
    else if (fsync(fileno(f)) != 0)     { fault(SD_DIAG_OP_FSYNC, errno); why = "manifest_write_failed"; }
    if (fclose(f) != 0 && why == NULL)  { fault(SD_DIAG_OP_CLOSE, errno); why = "manifest_write_failed"; }
    if (why == NULL) {
        e = ambit_fw_manifest_read(dir, &have, &have_len);
        bool same = e == ESP_OK && have_len == len && memcmp(have, json, len) == 0;
        free(have);
        if (!same) {
            fault(SD_DIAG_OP_VERIFY, EIO);
            why = "manifest_readback_failed";
        }
    }
    if (why != NULL) {
        sdcard_report_io_error();
        drop(path);
    }
    return why;
}

/* ── one region: HTTP → exclusive-create final name → verify ────────────── */

static const char *fetch_region(const ambit_fw_http_t *http, const char *url, const char *dir, size_t i,
                                const ambit_fw_region_t *r, uint8_t *buf, uint32_t *bytes)
{
    int64_t clen = -1;
    void *h = http->open(http->ctx, url, &clen);   /* TLS handshake outside any SD ref */
    if (h == NULL) return "region_http_failed";
    if (clen >= 0 && clen != (int64_t)r->size) {
        http->close(h);
        return "region_size_mismatch";
    }
    if (!sdcard_io_begin()) {
        http->close(h);
        return "sd_unavailable";
    }
    char path[192];
    snprintf(path, sizeof path, "%s/%s", dir, ambit_fw_region_name(i));
    FILE *f = fopen(path, "wx");                   /* exclusive: never truncates anything */
    if (f == NULL) {
        fault(SD_DIAG_OP_OPEN, errno);
        sdcard_report_io_error();
        sdcard_io_end();
        http->close(h);
        return "region_open_failed";
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    uint32_t total = 0;
    const char *why = NULL;
    for (;;) {
        int n = http->read(h, buf, FETCH_BUF);
        if (n < 0) { why = "region_read_failed"; break; }
        if (n == 0) break;
        if ((uint64_t)total + (uint64_t)n > r->size) { why = "region_too_long"; break; }
        errno = 0;
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            fault(SD_DIAG_OP_WRITE, errno);
            sdcard_report_io_error();
            why = "region_write_failed";
            break;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)n);
        total += (uint32_t)n;
    }
    uint8_t got[32];
    mbedtls_sha256_finish(&sha, got);
    mbedtls_sha256_free(&sha);
    if (why == NULL && total != r->size) why = "region_short";
    if (why == NULL && memcmp(got, r->sha256, sizeof got) != 0) why = "region_sha256_mismatch";
    if (why == NULL) {
        if (fflush(f) != 0)             { fault(SD_DIAG_OP_FLUSH, errno); why = "region_sync_failed"; }
        else if (fsync(fileno(f)) != 0) { fault(SD_DIAG_OP_FSYNC, errno); why = "region_sync_failed"; }
        if (why != NULL) sdcard_report_io_error();
    }
    /* fclose always releases the stream, even when it reports failure. */
    if (fclose(f) != 0 && why == NULL) {
        fault(SD_DIAG_OP_CLOSE, errno);
        sdcard_report_io_error();
        why = "region_close_failed";
    }
    if (why == NULL && ambit_fw_file_check(dir, i, r, true) != AMBIT_FW_FILE_OK) {
        fault(SD_DIAG_OP_VERIFY, EIO);
        why = "region_readback_failed";
    }
    if (why != NULL) drop(path);
    sdcard_io_end();
    http->close(h);
    if (why == NULL) *bytes += total;
    return why;
}

/* ── the fetch ──────────────────────────────────────────────────────────── */

static esp_err_t done(ambit_fw_fetch_result_t *res, const char *why)
{
    res->why = why ? why : "ok";
    return why ? ESP_FAIL : ESP_OK;
}

esp_err_t ambit_fw_fetch(const char *root, const char *manifest_url, uint8_t mj, uint8_t mn, uint8_t bt,
                         const ambit_fw_http_t *http, ambit_fw_fetch_result_t *res)
{
    memset(res, 0, sizeof *res);
    if (root == NULL || manifest_url == NULL || http == NULL) return done(res, "bad_args");
    const char *slash = strrchr(manifest_url, '/');
    const char *scheme = strstr(manifest_url, "://");
    if (slash == NULL || scheme == NULL || slash <= scheme + 2 || slash[1] == '\0') {
        return done(res, "bad_manifest_url");
    }
    const size_t base_len = (size_t)(slash - manifest_url) + 1;   /* through the '/' */

    char *json = NULL;
    size_t json_len = 0;
    const char *why = get_manifest(http, manifest_url, &json, &json_len);
    if (why != NULL) return done(res, why);

    ambit_fw_manifest_t m;
    const char *pwhy = NULL;
    if (ambit_fw_manifest_parse(json, json_len, &m, &pwhy) != ESP_OK) {
        free(json);
        return done(res, pwhy);   /* e.g. region_size_out_of_bounds */
    }
    if (m.major != mj || m.minor != mn || m.batch != bt) {
        free(json);
        return done(res, "manifest_version_mismatch");
    }

    char dir[96];
    snprintf(dir, sizeof dir, "%s/%u.%u.%u", root, mj, mn, bt);
    bool need[AMBIT_FW_NUM_REGIONS] = { false };

    /* Steps 2 + 3 in one SD bracket: drop non-matching regions, then commit the
     * manifest that gates everything written after it. */
    if (!sdcard_io_begin()) {
        free(json);
        return done(res, "sd_unavailable");
    }
    if ((mkdir(root, 0777) != 0 && errno != EEXIST) || (mkdir(dir, 0777) != 0 && errno != EEXIST)) {
        fault(SD_DIAG_OP_MKDIR, errno);
        why = "mkdir_failed";
    }
    for (size_t i = 0; i < AMBIT_FW_NUM_REGIONS && why == NULL; i++) {
        switch (ambit_fw_file_check(dir, i, &m.region[i], true)) {
        case AMBIT_FW_FILE_OK:
            res->reused++;
            break;
        case AMBIT_FW_FILE_MISSING:
            need[i] = true;
            break;
        case AMBIT_FW_FILE_MISMATCH: {
            char path[192];
            snprintf(path, sizeof path, "%s/%s", dir, ambit_fw_region_name(i));
            if (remove(path) != 0 && errno != ENOENT) {
                fault(SD_DIAG_OP_REMOVE, errno);
                why = "region_remove_failed";
                break;
            }
            res->removed++;
            need[i] = true;
            break;
        }
        case AMBIT_FW_FILE_UNREADABLE:
            why = "sd_read_failed";
            break;
        }
    }
    if (why == NULL) why = write_manifest(dir, json, json_len);
    sdcard_io_end();
    free(json);
    if (why != NULL) return done(res, why);

    /* Step 4: the missing regions, one HTTP GET each. */
    uint8_t *buf = malloc(FETCH_BUF);
    if (buf == NULL) return done(res, "no_mem");
    for (size_t i = 0; i < AMBIT_FW_NUM_REGIONS && why == NULL; i++) {
        if (!need[i]) continue;
        char url[AMBIT_FW_FETCH_URL_MAX];
        int n = snprintf(url, sizeof url, "%.*s%s", (int)base_len, manifest_url, m.region[i].src);
        if (n < 0 || (size_t)n >= sizeof url) {
            why = "url_too_long";
            break;
        }
        why = fetch_region(http, url, dir, i, &m.region[i], buf, &res->bytes);
        if (why == NULL) res->downloaded++;
    }
    free(buf);
    if (why != NULL) return done(res, why);

    /* Step 5: the folder as the flash preflight will see it. */
    if (!sdcard_io_begin()) return done(res, "sd_unavailable");
    const char *gwhy = NULL;
    ambit_fw_dir_t g = ambit_fw_dir_check(dir, true, &gwhy);
    sdcard_io_end();
    return done(res, g == AMBIT_FW_DIR_VERIFIED ? NULL : "final_verify_failed");
}
