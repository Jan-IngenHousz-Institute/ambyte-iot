/* Replacement-safe AMBIT OTA staging (see ambit_stage.h). */
#include "ambit_stage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mbedtls/sha256.h"
#include "sd_card.h"
#include "sd_diag.h"

/* Verification build only: `evq_hil fault io ambit_ota …` (LAST include). */
#define EVQ_HIL_WRITER SD_DIAG_W_AMBIT_OTA
#include "evq_hil_io_w.h"

#define STAGE_PREFIX "ambit_fw.stg-"
#define STAGE_SUFFIX ".bin"
#define STAGE_BUF    1024

static void fault(sd_diag_op_t op, int err)
{
    sd_diag_fault(SD_DIAG_W_AMBIT_OTA, op, err ? err : EIO);
}

const char *ambit_stage_why_name(ambit_stage_why_t why)
{
    static const char *const n[] = { "ok", "staging_slots_blocked", "open_failed", "source_failed", "short_source",
                                     "write_failed", "sync_failed", "close_failed", "readback_failed",
                                     "readback_mismatch", "empty" };
    return (unsigned)why < sizeof n / sizeof n[0] ? n[why] : "?";
}

void ambit_stage_cleanup(const char *dir, ambit_stage_cleanup_t *out)
{
    /* Stages are only ever created as <dir>/ambit_fw.stg-<k>.bin, k < SLOTS:
     * remove exactly those names (no directory scan). Each is untrusted — its
     * verified identity died with the attempt that wrote it — single-named and
     * never renamed, so unlinking it cannot free a cross-linked chain. */
    ambit_stage_cleanup_t c = { 0 };
    for (unsigned k = 0; k < AMBIT_STAGE_SLOTS; k++) {
        char p[128];
        snprintf(p, sizeof p, "%.80s/" STAGE_PREFIX "%u" STAGE_SUFFIX, dir, k);
        if (remove(p) == 0) c.stale_removed++;
        else if (errno != ENOENT) { c.stale_remove_err++; fault(SD_DIAG_OP_REMOVE, errno); }
    }
    if (out) *out = c;
}

static esp_err_t fail(ambit_stage_why_t w, ambit_stage_why_t *why, const char *path, bool drop)
{
    if (why) *why = w;
    if (drop && path != NULL && remove(path) != 0 && errno != ENOENT) fault(SD_DIAG_OP_REMOVE, errno);
    return ESP_FAIL;
}

enum { RV_OK = 0, RV_OPEN, RV_READ, RV_MISMATCH };

/* Re-read `path`, hashing every byte (and feeding `sink` when given), and
 * compare length + SHA-256 with what was verified before. */
static int read_verify(const char *path, size_t want_len, const uint8_t want_sha[32],
                       const ambit_stage_sink_t *sink, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { fault(SD_DIAG_OP_OPEN, errno); return RV_OPEN; }
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    mbedtls_sha256_starts(&h, 0);
    size_t got = 0, n;
    int rv = RV_OK;
    while ((n = fread(buf, 1, cap, f)) > 0) {
        mbedtls_sha256_update(&h, buf, n);
        got += n;
        if (got > want_len) { rv = RV_MISMATCH; break; }                 /* longer than verified */
        if (sink != NULL && !sink->data(sink->ctx, buf, n)) { rv = RV_READ; break; }
    }
    if (ferror(f)) { fault(SD_DIAG_OP_READ, EIO); rv = RV_READ; }
    fclose(f);
    uint8_t have[32];
    mbedtls_sha256_finish(&h, have);
    mbedtls_sha256_free(&h);
    if (rv == RV_OK && (got != want_len || memcmp(have, want_sha, sizeof have) != 0)) rv = RV_MISMATCH;
    if (rv == RV_MISMATCH) fault(SD_DIAG_OP_VERIFY, EIO);
    return rv;
}

esp_err_t ambit_stage_download(const char *dir, ambit_stage_read_fn rd, void *rd_ctx, int64_t expect_len,
                               ambit_stage_t *out, ambit_stage_why_t *why, ambit_stage_cleanup_t *cleanup)
{
    memset(out, 0, sizeof *out);
    ambit_stage_cleanup(dir, cleanup);

    FILE *f = NULL;
    for (unsigned k = 0; k < AMBIT_STAGE_SLOTS && f == NULL; k++) {
        snprintf(out->path, sizeof out->path, "%.64s/%s%u%s", dir, STAGE_PREFIX, k, STAGE_SUFFIX);
        f = fopen(out->path, "wx");            /* exclusive: never reuse or truncate a name */
        if (f == NULL && errno != EEXIST) {
            fault(SD_DIAG_OP_OPEN, errno);
            sdcard_report_io_error();
            out->path[0] = '\0';
            return fail(AMBIT_STAGE_OPEN_FAILED, why, NULL, false);
        }
    }
    if (f == NULL) {
        out->path[0] = '\0';
        return fail(AMBIT_STAGE_SLOTS_BLOCKED, why, NULL, false);
    }

    uint8_t buf[STAGE_BUF];
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    mbedtls_sha256_starts(&h, 0);
    size_t total = 0;
    ambit_stage_why_t w = AMBIT_STAGE_OK;
    for (;;) {
        int r = rd(rd_ctx, buf, sizeof buf);
        if (r < 0) { w = AMBIT_STAGE_SOURCE_FAILED; break; }
        if (r == 0) break;
        errno = 0;
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
            fault(SD_DIAG_OP_WRITE, errno);
            sdcard_report_io_error();
            w = AMBIT_STAGE_WRITE_FAILED;
            break;
        }
        mbedtls_sha256_update(&h, buf, (size_t)r);
        total += (size_t)r;
    }
    uint8_t want[32];
    mbedtls_sha256_finish(&h, want);
    mbedtls_sha256_free(&h);
    if (w == AMBIT_STAGE_OK && expect_len > 0 && (int64_t)total != expect_len) w = AMBIT_STAGE_SHORT_SOURCE;
    if (w == AMBIT_STAGE_OK && total == 0) w = AMBIT_STAGE_EMPTY;
    if (w == AMBIT_STAGE_OK) {
        if (fflush(f) != 0) { fault(SD_DIAG_OP_FLUSH, errno); w = AMBIT_STAGE_SYNC_FAILED; }
        else if (fsync(fileno(f)) != 0) { fault(SD_DIAG_OP_FSYNC, errno); w = AMBIT_STAGE_SYNC_FAILED; }
        if (w != AMBIT_STAGE_OK) sdcard_report_io_error();
    }
    /* fclose always releases the stream, even when it reports failure: the
     * handle is never touched again on any path below. */
    if (fclose(f) != 0 && w == AMBIT_STAGE_OK) {
        fault(SD_DIAG_OP_CLOSE, errno);
        sdcard_report_io_error();
        w = AMBIT_STAGE_CLOSE_FAILED;
    }
    f = NULL;
    if (w != AMBIT_STAGE_OK) return fail(w, why, out->path, true);

    /* Read back every byte by the stage's own name. */
    int rv = read_verify(out->path, total, want, NULL, buf, sizeof buf);
    if (rv == RV_OPEN || rv == RV_READ) return fail(AMBIT_STAGE_READBACK_FAILED, why, out->path, true);
    if (rv == RV_MISMATCH) {
        /* Written, synced, closed — and reads back different: keep the bytes
         * as write-corruption evidence (the next attempt clears it). */
        return fail(AMBIT_STAGE_MISMATCH, why, NULL, false);
    }
    memcpy(out->sha256, want, sizeof want);
    out->len = total;
    if (why) *why = AMBIT_STAGE_OK;
    return ESP_OK;
}

bool ambit_stage_stream(const ambit_stage_t *st, const ambit_stage_sink_t *sink, uint8_t *buf, size_t cap)
{
    if (st == NULL || st->len == 0 || st->path[0] == '\0') return false;
    if (!sink->begin(sink->ctx, st->len)) return false;
    /* Any mismatch means the card handed back different bytes than it
     * verified minutes ago: never let the target commit them. */
    if (read_verify(st->path, st->len, st->sha256, sink, buf, cap) != RV_OK) {
        sink->abort(sink->ctx);
        return false;
    }
    return sink->end(sink->ctx);
}

bool ambit_stage_remove(const ambit_stage_t *st)
{
    if (st == NULL || st->path[0] == '\0') return true;
    if (remove(st->path) == 0 || errno == ENOENT) return true;
    fault(SD_DIAG_OP_REMOVE, errno);
    return false;   /* left for the next attempt's step 1 */
}
