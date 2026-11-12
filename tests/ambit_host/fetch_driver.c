/* Scripted driver for the AMBIT release fetch + manifest gate under host test
 * (tests/test_ambit_fw_fetch.py). Compiles the production
 * components/ambit_ota/ambit_fw_fetch.c, components/ambit_flash/ambit_fw_manifest.c
 * and components/ambit_flash/ambit_flash_preflight.c through the SD media shim
 * (fault injection, io-gate accounting, simulated CPU reset); the HTTP transport
 * is a table of URL -> host file, with per-URL failure knobs.
 *
 * One command per stdin line, one JSON object per stdout line:
 *   serve <url> <host-file> [fail_at=-1] [clen=-2(-2: real size, -1: unknown)] [status=200]
 *   fetch <M.m.b> <manifest-url>
 *   gate <dir> <deep 0|1>
 *   preflight <dir>
 *   parse <host-file>
 *   arm <op> <mode> [nth] [count] [path-substring]
 *   status | quit */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ambit_flash_preflight.h"
#include "ambit_fw_fetch.h"
#include "ambit_fw_manifest.h"
#include "esp_err.h"
#include "sd_card.h"
#include "sd_diag.h"
#include "sd_shim.h"

#undef fopen
#undef fclose
#undef fread
#undef mkdir

extern FILE *g_console;

/* ── scripted HTTP ── */
#define MAX_URLS 16
typedef struct {
    char     url[300];
    uint8_t *data;
    size_t   len;
    long     fail_at;   /* read error once this many bytes were served (-1 never) */
    long     clen;      /* announced Content-Length (-1 unknown) */
    int      status;    /* != 200: open fails */
    unsigned opens;
} served_t;
static served_t s_srv[MAX_URLS];
static unsigned s_opens_total;

typedef struct {
    served_t *s;
    size_t    pos;
} conn_t;

static void *h_open(void *ctx, const char *url, int64_t *clen)
{
    (void)ctx;
    s_opens_total++;
    for (int i = 0; i < MAX_URLS; i++) {
        if (s_srv[i].data != NULL && strcmp(s_srv[i].url, url) == 0) {
            s_srv[i].opens++;
            if (s_srv[i].status != 200) return NULL;
            conn_t *c = calloc(1, sizeof *c);
            c->s = &s_srv[i];
            *clen = s_srv[i].clen;
            return c;
        }
    }
    return NULL;   /* 404 */
}

static int h_read(void *h, uint8_t *buf, size_t cap)
{
    conn_t *c = h;
    if (c->s->fail_at >= 0 && c->pos >= (size_t)c->s->fail_at) return -1;
    size_t n = c->s->len - c->pos;
    if (n > cap) n = cap;
    if (n > 700) n = 700;                                     /* uneven chunks */
    if (c->s->fail_at >= 0 && c->pos + n > (size_t)c->s->fail_at) n = (size_t)c->s->fail_at - c->pos;
    memcpy(buf, c->s->data + c->pos, n);
    c->pos += n;
    return (int)n;
}

static void h_close(void *h) { free(h); }

static const ambit_fw_http_t s_http = { .ctx = NULL, .open = h_open, .read = h_read, .close = h_close };

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    uint8_t *b = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) { cap = cap ? cap * 2 : 4096; b = realloc(b, cap); }
        size_t r = fread(b + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b ? b : malloc(1);
}

int main(int argc, char **argv)
{
    if (argc < 2 || chdir(argv[1]) != 0) { fprintf(stderr, "usage: fetch_driver <state-dir>\n"); return 2; }
    mkdir("sdcard", 0777);
    shim_open_log("ops.jsonl");
    g_console = stderr;
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32] = "";
        sscanf(line, "%31s", cmd);
        if (strcmp(cmd, "serve") == 0) {
            char url[300] = "", file[512] = "";
            long fail_at = -1, clen = -2;
            int status = 200;
            sscanf(line, "%*s %299s %511s %ld %ld %d", url, file, &fail_at, &clen, &status);
            int slot = -1;
            for (int i = 0; i < MAX_URLS && slot < 0; i++) {
                if (s_srv[i].data != NULL && strcmp(s_srv[i].url, url) == 0) slot = i;
            }
            for (int i = 0; i < MAX_URLS && slot < 0; i++) {
                if (s_srv[i].data == NULL) slot = i;
            }
            served_t *s = &s_srv[slot];
            free(s->data);
            memset(s, 0, sizeof *s);
            snprintf(s->url, sizeof s->url, "%s", url);
            s->data = slurp(file, &s->len);
            s->fail_at = fail_at;
            s->clen = clen == -2 ? (long)s->len : clen;
            s->status = status;
            printf("{\"serve\":\"%s\",\"len\":%zu}\n", s->data ? "ok" : "missing", s->len);
        } else if (strcmp(cmd, "fetch") == 0) {
            char ver[32] = "", url[300] = "";
            unsigned a = 0, b = 0, c = 0;
            sscanf(line, "%*s %31s %299s", ver, url);
            sscanf(ver, "%u.%u.%u", &a, &b, &c);
            for (int i = 0; i < MAX_URLS; i++) s_srv[i].opens = 0;
            ambit_fw_fetch_result_t r;
            esp_err_t rc = ambit_fw_fetch("./sdcard/ambit_fw", url, (uint8_t)a, (uint8_t)b, (uint8_t)c, &s_http, &r);
            unsigned opens = 0;
            for (int i = 0; i < MAX_URLS; i++) opens += s_srv[i].opens;
            printf("{\"rc\":%d,\"why\":\"%s\",\"downloaded\":%u,\"reused\":%u,\"removed\":%u,\"bytes\":%lu,"
                   "\"opens\":%u,\"refs\":%d}\n",
                   rc, r.why, r.downloaded, r.reused, r.removed, (unsigned long)r.bytes, opens, g_sd_refs);
        } else if (strcmp(cmd, "gate") == 0) {
            char dir[300] = "";
            int deep = 1;
            sscanf(line, "%*s %299s %d", dir, &deep);
            const char *why = NULL;
            sdcard_io_begin();
            ambit_fw_dir_t g = ambit_fw_dir_check(dir, deep != 0, &why);
            sdcard_io_end();
            static const char *const names[] = { "legacy", "verified", "rejected", "unreadable" };
            printf("{\"gate\":\"%s\",\"why\":\"%s\"}\n", names[g], why ? why : "");
        } else if (strcmp(cmd, "preflight") == 0) {
            char dir[300] = "";
            sscanf(line, "%*s %299s", dir);
            const char *fnames[AMBIT_FW_NUM_REGIONS];
            for (size_t i = 0; i < AMBIT_FW_NUM_REGIONS; i++) fnames[i] = ambit_fw_region_name(i);
            esp_err_t rc = ambit_flash_preflight("./sdcard/ambit_fw", dir, fnames, AMBIT_FW_NUM_REGIONS);
            printf("{\"rc\":%d,\"refs\":%d}\n", rc, g_sd_refs);
        } else if (strcmp(cmd, "parse") == 0) {
            char file[512] = "";
            sscanf(line, "%*s %511s", file);
            size_t len = 0;
            uint8_t *b = slurp(file, &len);
            ambit_fw_manifest_t m;
            const char *why = NULL;
            esp_err_t rc = b ? ambit_fw_manifest_parse((const char *)b, len, &m, &why) : ESP_FAIL;
            printf("{\"rc\":%d,\"why\":\"%s\",\"version\":\"%u.%u.%u\",\"sizes\":[%lu,%lu,%lu,%lu],"
                   "\"src\":[\"%s\",\"%s\",\"%s\",\"%s\"]}\n",
                   rc, why ? why : "", m.major, m.minor, m.batch, (unsigned long)m.region[0].size,
                   (unsigned long)m.region[1].size, (unsigned long)m.region[2].size, (unsigned long)m.region[3].size,
                   m.region[0].src, m.region[1].src, m.region[2].src, m.region[3].src);
            free(b);
        } else if (strcmp(cmd, "arm") == 0) {
            char op[32], mode[32], sub[128] = "";
            unsigned nth = 1, count = 1;
            int k = sscanf(line, "%*s %31s %31s %u %u %127s", op, mode, &nth, &count, sub);
            shim_mode_t m;
            int ok = k >= 2 && shim_parse_mode(mode, &m) == 0 && shim_arm(op, k >= 5 ? sub : NULL, m, nth, count) == 0;
            printf("{\"arm\":\"%s\"}\n", ok ? "ok" : "bad");
        } else if (strcmp(cmd, "status") == 0) {
            sd_diag_block_t db;
            sd_diag_get(&db);
            char dj[2048] = "{}";
            sd_diag_render_json(&db, dj, sizeof dj);
            printf("{\"violations\":%u,\"refs\":%d,\"io_errors\":%u,\"diag\":%s}\n", shim_violations(), g_sd_refs,
                   g_sd_io_errors, dj);
        } else if (strcmp(cmd, "quit") == 0) {
            break;
        } else if (cmd[0]) {
            printf("{\"error\":\"unknown %s\"}\n", cmd);
        }
        fflush(stdout);
    }
    return 0;
}
