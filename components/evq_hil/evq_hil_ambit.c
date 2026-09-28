/* AMBIT update controls for the Sprint 2 replacement bench (amendment A6).
 * Verification build only (CONFIG_AMBYTE_EVQ_HIL; see CMakeLists.txt).
 *
 *   evq_hil ambit_sync <hold|release|status>
 *   evq_hil ambit_nvsdump <ch> <tag>
 *   evq_hil ambit_stage <ver> <region> <url> <sha256> <size>
 *   evq_hil ambit_stage commit <ver> <sha_bootloader> <sha_partitions> <sha_boot_app0> <sha_app>
 *
 * Staging writes each region straight to its final name inside the hidden
 * folder /sdcard/ambit_fw/.s2-<ver>/ (never a stage-then-rename per file, never
 * over an existing file, never unlinks anything): ambit_flash_find_target only
 * accepts <M>.<m>.<b> folder names, so nothing can flash from the folder until
 * `commit` has re-hashed all four files against the manifest SHAs and renamed
 * the folder to <ver>. A download or read-back that fails moves the partial
 * file aside to <region>.bad-<n> (evidence), so a retry starts clean. Existing
 * SD content (the unreleased 1.0.0 folder included) is never touched. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ambit_flash.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "evq_hil_sdl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"

#define EVQ_HIL_AMB_ROOT "/sdcard/ambit_fw"
#define EVQ_HIL_AMB_MAX  (2u * 1024u * 1024u)

static const char *const evq_hil_amb_regions[4] = { "bootloader.bin", "partitions.bin", "boot_app0.bin", "app.bin" };
static uint8_t evq_hil_amb_buf[4096];

static bool evq_hil_amb_ver_ok(const char *v)
{
    unsigned a, b, c;
    char tail;
    return strlen(v) < 12 && sscanf(v, "%u.%u.%u%c", &a, &b, &c, &tail) == 3 && a < 256 && b < 256 && c < 256;
}

static int evq_hil_amb_region_idx(const char *r)
{
    for (int i = 0; i < 4; i++) if (strcmp(r, evq_hil_amb_regions[i]) == 0) return i;
    return -1;
}

static bool evq_hil_amb_hex_ok(const char *h)
{
    if (strlen(h) != 64) return false;
    for (const char *p = h; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

/* sha256 + length of a file; 0 ok, -1 open/read error. Caller holds the SD ref. */
static int evq_hil_amb_hash_file(const char *path, char hex[65], long *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    hil_sdl_sha_ctx_t c;
    hil_sdl_sha256_init(&c);
    long n = 0;
    size_t r;
    while ((r = fread(evq_hil_amb_buf, 1, sizeof evq_hil_amb_buf, f)) > 0) {
        hil_sdl_sha256_update(&c, evq_hil_amb_buf, r);
        n += (long)r;
    }
    int err = ferror(f);
    fclose(f);
    if (err) return -1;
    uint8_t d[32];
    hil_sdl_sha256_final(&c, d);
    hil_sdl_hex(d, sizeof d, hex);
    *len = n;
    return 0;
}

/* Move a failed file aside (never unlinked): <path>.bad-<n>, first free n. */
static void evq_hil_amb_aside(const char *path)
{
    char to[160];
    struct stat st;
    for (int n = 0; n < 100; n++) {
        snprintf(to, sizeof to, "%s.bad-%d", path, n);
        if (stat(to, &st) != 0) {
            int rc = rename(path, to);
            printf("SLT_AMBSTG aside %s -> %s rc=%d\n", path, to, rc);
            return;
        }
    }
    printf("SLT_AMBSTG aside %s refused (no free .bad-<n>)\n", path);
}

/* HTTP GET (bounded redirects, cert bundle) streamed into the open fd. */
static int evq_hil_amb_fetch(const char *url, int fd, long want, long *got, char hex[65], const char **why)
{
    esp_http_client_config_t cfg = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000,
        .buffer_size = 4096, .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) { *why = "http_init"; return -1; }
    int rc = -1;
    if (esp_http_client_open(h, 0) != ESP_OK) { *why = "http_open"; goto out; }
    int64_t clen = esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    for (int hops = 0; hops < 3 && status >= 301 && status <= 308 && status != 304; hops++) {
        if (esp_http_client_set_redirection(h) != ESP_OK) break;
        esp_http_client_close(h);
        if (esp_http_client_open(h, 0) != ESP_OK) { *why = "http_open"; goto out; }
        clen = esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
    }
    if (status != 200) { *why = "http_status"; printf("SLT_AMBSTG http status=%d\n", status); goto out; }
    if (clen > 0 && clen != want) { *why = "length"; printf("SLT_AMBSTG content_length=%lld want=%ld\n", (long long)clen, want); goto out; }
    hil_sdl_sha_ctx_t c;
    hil_sdl_sha256_init(&c);
    long n = 0;
    for (;;) {
        int r = esp_http_client_read(h, (char *)evq_hil_amb_buf, sizeof evq_hil_amb_buf);
        vTaskDelay(1);
        if (r < 0) { *why = "http_read"; goto out; }
        if (r == 0) break;
        if (n + r > want) { *why = "too_long"; goto out; }
        hil_sdl_sha256_update(&c, evq_hil_amb_buf, (size_t)r);
        for (int off = 0; off < r;) {
            ssize_t w = write(fd, evq_hil_amb_buf + off, (size_t)(r - off));
            if (w <= 0) { *why = "write"; goto out; }
            off += (int)w;
        }
        n += r;
    }
    uint8_t d[32];
    hil_sdl_sha256_final(&c, d);
    hil_sdl_hex(d, sizeof d, hex);
    *got = n;
    rc = 0;
out:
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return rc;
}

static int evq_hil_amb_stage(const char *ver, const char *region, const char *url, const char *sha, long size)
{
    char dir[64], path[160], hex[65] = "-", rb[65] = "-";
    snprintf(dir, sizeof dir, EVQ_HIL_AMB_ROOT "/.s2-%s", ver);
    snprintf(path, sizeof path, "%s/%s", dir, region);
    if (!sdcard_io_begin()) { printf("SLT_AMBSTG %s %s refused sd_unavailable\n", ver, region); return 1; }
    int rc = 1;
    long got = 0, rlen = 0;
    const char *why = "ok";
    struct stat st;
    (void)mkdir(EVQ_HIL_AMB_ROOT, 0775);
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) { why = "mkdir"; goto done; }
    if (stat(path, &st) == 0) {
        /* Idempotent re-run: an existing file is only ever reported, never replaced. */
        if (evq_hil_amb_hash_file(path, rb, &rlen) == 0 && rlen == size && strcmp(rb, sha) == 0) {
            printf("SLT_AMBSTG %s %s present size=%ld sha=%s ok\n", ver, region, rlen, rb);
            rc = 0;
        } else {
            printf("SLT_AMBSTG %s %s refused exists size=%ld sha=%s (move it aside by hand)\n", ver, region, rlen, rb);
        }
        goto done;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (fd < 0) { why = "open"; goto done; }
    int fr = evq_hil_amb_fetch(url, fd, size, &got, hex, &why);
    if (fr == 0 && fsync(fd) != 0) { fr = -1; why = "fsync"; }
    if (close(fd) != 0 && fr == 0) { fr = -1; why = "close"; }
    if (fr == 0 && (got != size || strcmp(hex, sha) != 0)) { fr = -1; why = "stream_mismatch"; }
    if (fr == 0 && (evq_hil_amb_hash_file(path, rb, &rlen) != 0 || rlen != size || strcmp(rb, sha) != 0)) {
        fr = -1;
        why = "readback_mismatch";
    }
    if (fr != 0) {
        evq_hil_amb_aside(path);
        goto done;
    }
    printf("SLT_AMBSTG %s %s staged size=%ld sha=%s readback=%s ok\n", ver, region, rlen, hex, rb);
    rc = 0;
done:
    if (rc != 0 && strcmp(why, "ok") != 0) {
        printf("SLT_AMBSTG %s %s failed why=%s errno=%d got=%ld sha=%s\n", ver, region, why, errno, got, hex);
    }
    sdcard_io_end();
    return rc;
}

static int evq_hil_amb_commit(const char *ver, char **shas)
{
    char dir[64], to[64], path[160], hex[65];
    snprintf(dir, sizeof dir, EVQ_HIL_AMB_ROOT "/.s2-%s", ver);
    snprintf(to, sizeof to, EVQ_HIL_AMB_ROOT "/%s", ver);
    if (!sdcard_io_begin()) { printf("SLT_AMBSTG commit %s refused sd_unavailable\n", ver); return 1; }
    int bad = 0;
    struct stat st;
    if (stat(to, &st) == 0) {
        printf("SLT_AMBSTG commit %s refused target_exists\n", ver);
        sdcard_io_end();
        return 1;
    }
    for (int i = 0; i < 4; i++) {
        long len = 0;
        snprintf(path, sizeof path, "%s/%s", dir, evq_hil_amb_regions[i]);
        int r = evq_hil_amb_hash_file(path, hex, &len);
        bool ok = r == 0 && strcmp(hex, shas[i]) == 0;
        printf("SLT_AMBSTG commit %s %s size=%ld sha=%s %s\n", ver, evq_hil_amb_regions[i], len, r == 0 ? hex : "-",
               ok ? "ok" : "mismatch");
        bad += !ok;
    }
    int rc = 1;
    if (bad == 0) {
        rc = rename(dir, to) == 0 ? 0 : 1;
        printf("SLT_AMBSTG commit %s %s -> %s %s errno=%d\n", ver, dir, to, rc == 0 ? "ok" : "rename_failed", rc ? errno : 0);
    } else {
        printf("SLT_AMBSTG commit %s refused mismatches=%d (folder stays hidden)\n", ver, bad);
    }
    sdcard_io_end();
    return rc;
}

/* Returns -1 when `sub` is not an ambit_* command. */
int evq_hil_ambit_cmd(int argc, char **argv)
{
    const char *sub = argv[1];
    if (strcmp(sub, "ambit_sync") == 0 && argc >= 3) {
        if (strcmp(argv[2], "release") == 0) { evq_hil_ambit_sync_set(true); return 0; }
        if (strcmp(argv[2], "hold") == 0) { evq_hil_ambit_sync_set(false); return 0; }
        if (strcmp(argv[2], "status") == 0) {
            printf("SLT_AMB sync=%s\n", evq_hil_ambit_sync_held() ? "held" : "released");
            return 0;
        }
    }
    if (strcmp(sub, "ambit_nvsdump") == 0 && argc >= 4) {
        int ch = atoi(argv[2]);
        const char *tag = argv[3];
        bool tag_ok = strlen(tag) >= 1 && strlen(tag) <= 8;
        for (const char *p = tag; *p; p++)
            tag_ok &= (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
        if (ch < 0 || ch > 3 || !tag_ok) { printf("SLT_AMB_ERR usage: ambit_nvsdump <0..3> <tag [A-Za-z0-9]{1,8}>\n"); return 1; }
        return evq_hil_ambit_nvsdump((uint8_t)ch, tag) == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "ambit_stage") == 0) {
        if (argc == 8 && strcmp(argv[2], "commit") == 0) {
            if (!evq_hil_amb_ver_ok(argv[3])) { printf("SLT_AMBSTG refused bad_version\n"); return 1; }
            for (int i = 4; i < 8; i++) if (!evq_hil_amb_hex_ok(argv[i])) { printf("SLT_AMBSTG refused bad_sha\n"); return 1; }
            return evq_hil_amb_commit(argv[3], &argv[4]);
        }
        if (argc == 7) {
            long size = atol(argv[6]);
            if (!evq_hil_amb_ver_ok(argv[2]) || evq_hil_amb_region_idx(argv[3]) < 0 || strncmp(argv[4], "https://", 8) != 0 ||
                !evq_hil_amb_hex_ok(argv[5]) || size <= 0 || size > (long)EVQ_HIL_AMB_MAX) {
                printf("SLT_AMBSTG refused bad_args\n");
                return 1;
            }
            return evq_hil_amb_stage(argv[2], argv[3], argv[4], argv[5], size);
        }
        printf("SLT_AMBSTG usage: ambit_stage <ver> <region> <https-url> <sha256> <size> | "
               "ambit_stage commit <ver> <sha_bl> <sha_pt> <sha_ba> <sha_app>\n");
        return 1;
    }
    return -1;
}
