/* evq_hil sdlog_* commands (Sprint 2 H3 emitter, H4 inventory, H5 export,
 * H7 quiesce, H2/H8 trace control), verification build only. Wire format:
 * docs/sdlog-hil-trace.md.
 *
 * Why the quiesce: an inventory or export of /sdcard/logs is only coherent if
 * the logger cannot append, roll back or rotate while a file is read. The
 * production pause handshake (the same one the low-battery guard uses) drains
 * the ring, commits, closes and HOLDS the writer; producers keep buffering into
 * the RAM ring (overflow is counted and traced). Each file is then opened, read
 * to EOF and closed inside ONE sdcard_io_begin/end hold, so no FILE* ever
 * survives a gate release (a card teardown cannot free the volume under us). */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "device_commands.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "evq_hil.h"
#include "evq_hil_payload.h"
#include "evq_hil_sdl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"
#include "sd_logger.h"

#define HIL_SDL_LOGDIR    SD_MOUNT_POINT "/logs"
#define HIL_SDL_MAXFILES  16
#define HIL_SDL_RDBUF     4096
#define HIL_SDL_B64CHUNK  3072                 /* multiple of 3: whole base64 groups per line */
#define HIL_SDL_MAXLINES  20000
#define HIL_SDL_LINE_MAX  256

static uint8_t      *hil_sdl_rbuf;             /* PSRAM read buffer (4 KiB) */
static uint8_t      *hil_sdl_dbuf;             /* PSRAM dump accumulation (3 KiB) */
static char         *hil_sdl_obuf;             /* PSRAM base64 line (4 KiB) */
static volatile bool hil_sdl_busy;             /* one inventory/dump at a time */
static volatile bool hil_sdl_emit_busy;

static bool hil_sdl_u32(const char *s, uint32_t *out)
{
    char *e = NULL;
    unsigned long v = strtoul(s, &e, 10);
    if (s[0] == '\0' || *e != '\0' || v > 0xFFFFFFFFul) return false;
    *out = (uint32_t)v;
    return true;
}

static bool hil_sdl_bufs(void)
{
    if (hil_sdl_rbuf == NULL) hil_sdl_rbuf = heap_caps_malloc(HIL_SDL_RDBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (hil_sdl_dbuf == NULL) hil_sdl_dbuf = heap_caps_malloc(HIL_SDL_B64CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (hil_sdl_obuf == NULL) hil_sdl_obuf = heap_caps_malloc(4096 + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return hil_sdl_rbuf && hil_sdl_dbuf && hil_sdl_obuf;
}

/* ── H7 quiesce ──────────────────────────────────────────────────────────── */
static int hil_sdl_quiesce(uint32_t *gen)
{
    power_reading_t p;
    cmd_result_t r = cmd_read_power(&p);
    if (r.status != ESP_OK || !p.input_present || hil_sdl_guard_parked()) {
        hil_sdl_ev(HIL_SDL_QUI, HIL_SDL_QUI_REFUSED, 0, 0, NULL);
        printf("SDL_Q refused guard\n");
        return -1;
    }
    *gen = hil_sdl_guard_gen();
    sd_logger_pause();
    for (int i = 0; i < 100 && !sd_logger_hil_paused(); i++) vTaskDelay(pdMS_TO_TICKS(100));   /* ≤ 10 s */
    if (!sd_logger_hil_paused()) {
        sd_logger_resume();
        hil_sdl_ev(HIL_SDL_QUI, HIL_SDL_QUI_TIMEOUT, 0, 0, NULL);
        printf("SDL_Q timeout\n");
        return -1;
    }
    hil_sdl_ev(HIL_SDL_QUI, HIL_SDL_QUI_PAUSED, 0, 0, NULL);
    printf("SDL_Q paused\n");
    return 0;
}

static void hil_sdl_resume(void)
{
    sd_logger_resume();
    hil_sdl_ev(HIL_SDL_QUI, HIL_SDL_QUI_RESUMED, 0, 0, NULL);
    printf("SDL_Q resumed\n");
}

/* ── listing (inside one gate hold) ─────────────────────────────────────── */
static int hil_sdl_list(char names[][32], int max)
{
    if (!sdcard_io_begin()) return -1;
    int n = 0;
    DIR *d = opendir(HIL_SDL_LOGDIR);
    if (d != NULL) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL && n < max) {
            if (de->d_name[0] == '.' || strlen(de->d_name) >= 32) continue;
            char p[64];
            struct stat st;
            snprintf(p, sizeof p, "%s/%.31s", HIL_SDL_LOGDIR, de->d_name);
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            snprintf(names[n++], 32, "%.31s", de->d_name);
        }
        closedir(d);
    }
    sdcard_io_end();
    for (int i = 1; i < n; i++) {                     /* insertion sort by name */
        char t[32];
        memcpy(t, names[i], 32);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], t) > 0) { memcpy(names[j + 1], names[j], 32); j--; }
        memcpy(names[j + 1], t, 32);
    }
    return n;
}

/* ── whole-file read inside ONE gate hold ───────────────────────────────── */
typedef int (*hil_sdl_cb_t)(const uint8_t *p, size_t n, uint32_t off, void *ctx);

/* rc 0 ok, -1 invalid (why set), -2 timeout. `per_mib_us` = hold budget per MiB (min 1 MiB). */
static int hil_sdl_read(const char *name, int64_t per_mib_us, hil_sdl_cb_t cb, void *ctx, uint32_t *size_out,
                        const char **why)
{
    char path[64];
    snprintf(path, sizeof path, "%s/%.31s", HIL_SDL_LOGDIR, name);
    if (!sdcard_io_begin()) { *why = "gate"; return -1; }
    int rc = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) { *why = "open"; rc = -1; goto out; }
    struct stat st;
    if (fstat(fileno(f), &st) != 0) { *why = "stat"; rc = -1; fclose(f); goto out; }
    *size_out = (uint32_t)st.st_size;
    int64_t mib = ((int64_t)st.st_size + (1 << 20) - 1) >> 20;
    int64_t budget = per_mib_us * (mib < 1 ? 1 : mib);
    int64_t t0 = esp_timer_get_time();
    uint32_t off = 0;
    for (;;) {
        size_t n = fread(hil_sdl_rbuf, 1, HIL_SDL_RDBUF, f);
        if (n == 0) {
            if (ferror(f)) { *why = "read"; rc = -1; }
            break;
        }
        rc = cb(hil_sdl_rbuf, n, off, ctx);
        if (rc != 0) { *why = "cb"; break; }
        off += (uint32_t)n;
        if (esp_timer_get_time() - t0 > budget) { rc = -2; break; }
    }
    if (rc == 0 && off != *size_out) { *why = "size_changed"; rc = -1; }   /* paused writer: must not happen */
    fclose(f);
out:
    sdcard_io_end();
    return rc;
}

/* ── H4 inventory ───────────────────────────────────────────────────────── */
typedef struct {
    hil_sdl_sha_ctx_t whole, line;
    uint32_t lines, maxline, cur_len, line_start, from_off, emitted, next_off;
    bool     per_line, more;
    uint8_t  last;
    const char *name;
} hil_sdl_inv_t;

static void hil_sdl_line_done(hil_sdl_inv_t *v, bool terminated)
{
    uint32_t len = v->cur_len;
    if (terminated) v->lines++;
    if (len > v->maxline) v->maxline = len;
    if (v->per_line && v->line_start >= v->from_off && len > 0) {
        if (v->emitted < HIL_SDL_MAXLINES) {
            uint8_t d[32];
            char hex[65];
            hil_sdl_sha256_final(&v->line, d);
            hil_sdl_hex(d, 32, hex);
            printf("SDL_FL %s %u %u %s\n", v->name, (unsigned)v->line_start, (unsigned)len, hex);
            v->emitted++;
        } else if (!v->more) {
            v->more = true;
            v->next_off = v->line_start;
        }
    }
    v->line_start += len;
    v->cur_len = 0;
    hil_sdl_sha256_init(&v->line);
}

static int hil_sdl_inv_cb(const uint8_t *p, size_t n, uint32_t off, void *ctx)
{
    (void)off;
    hil_sdl_inv_t *v = ctx;
    hil_sdl_sha256_update(&v->whole, p, n);
    size_t s = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') {
            hil_sdl_sha256_update(&v->line, p + s, i + 1 - s);
            v->cur_len += (uint32_t)(i + 1 - s);
            hil_sdl_line_done(v, true);
            s = i + 1;
        }
    }
    if (s < n) {
        hil_sdl_sha256_update(&v->line, p + s, n - s);
        v->cur_len += (uint32_t)(n - s);
    }
    v->last = p[n - 1];
    return 0;
}

static int hil_sdl_inv(const char *one, uint32_t from_off)
{
    char names[HIL_SDL_MAXFILES][32];
    int nf = hil_sdl_list(names, HIL_SDL_MAXFILES);
    if (nf < 0) { printf("SDL_INVALID - gate\n"); return 1; }
    int64_t t_end = esp_timer_get_time() + 180LL * 1000000;
    uint32_t gen0 = hil_sdl_guard_gen();
    int shown = 0, rc = 0;
    for (int i = 0; i < nf; i++) {
        if (one != NULL && strcmp(one, names[i]) != 0) continue;
        hil_sdl_inv_t v;
        memset(&v, 0, sizeof v);
        hil_sdl_sha256_init(&v.whole);
        hil_sdl_sha256_init(&v.line);
        v.name = names[i];
        v.per_line = one != NULL;
        v.from_off = from_off;
        uint32_t size = 0;
        const char *why = "-";
        int r = hil_sdl_read(names[i], 3LL * 1000000, hil_sdl_inv_cb, &v, &size, &why);
        if (r == 0 && v.cur_len > 0) hil_sdl_line_done(&v, false);          /* unterminated tail */
        if (r == -2) { printf("SDL_TIMEOUT %s\n", names[i]); rc = 1; break; }
        if (r != 0) { printf("SDL_INVALID %s %s\n", names[i], why); rc = 1; break; }
        if (hil_sdl_guard_gen() != gen0) { printf("SDL_INVALID %s guard\n", names[i]); rc = 1; break; }
        if (one == NULL) {
            uint8_t d[32];
            char hex[65];
            hil_sdl_sha256_final(&v.whole, d);
            hil_sdl_hex(d, 32, hex);
            bool torn = (size > 0 && v.last != '\n') || v.maxline > HIL_SDL_LINE_MAX;
            printf("SDL_FF %s %u %s %u %d %u\n", names[i], (unsigned)size, hex, (unsigned)v.lines, torn ? 1 : 0,
                   (unsigned)v.maxline);
        } else if (v.more) {
            printf("SDL_MORE %u\n", (unsigned)v.next_off);
        }
        shown++;
        if (esp_timer_get_time() > t_end) { printf("SDL_TIMEOUT -\n"); rc = 1; break; }
        vTaskDelay(1);
    }
    if (rc == 0 && one == NULL) {
        uint32_t fb = 0, cm = 0, bo = 0;
        bool q = false, open = false;
        sd_logger_hil_state(&fb, &cm, &q, &bo, &open);
        printf("SDL_STATE ambyte.log %u %d %u\n", (unsigned)cm, q ? 1 : 0, (unsigned)bo);
    }
    if (rc == 0) printf("SDL_INV_END %d\n", shown);
    return rc;
}

/* ── H5 export ──────────────────────────────────────────────────────────── */
typedef struct {
    hil_sdl_sha_ctx_t whole, range;
    uint32_t from_off, fill, fill_off;
} hil_sdl_dump_t;

static void hil_sdl_dump_flush(hil_sdl_dump_t *v)
{
    if (v->fill == 0) return;
    (void)hil_sdl_b64(hil_sdl_dbuf, v->fill, hil_sdl_obuf, 4096 + 64);
    printf("SDL_B64 %u %s\n", (unsigned)v->fill_off, hil_sdl_obuf);
    v->fill_off += v->fill;
    v->fill = 0;
}

static int hil_sdl_dump_cb(const uint8_t *p, size_t n, uint32_t off, void *ctx)
{
    hil_sdl_dump_t *v = ctx;
    hil_sdl_sha256_update(&v->whole, p, n);
    size_t i = off >= v->from_off ? 0 : (v->from_off - off < n ? v->from_off - off : n);
    while (i < n) {
        if (v->fill == 0) v->fill_off = off + (uint32_t)i;
        size_t take = HIL_SDL_B64CHUNK - v->fill;
        if (take > n - i) take = n - i;
        memcpy(hil_sdl_dbuf + v->fill, p + i, take);
        hil_sdl_sha256_update(&v->range, p + i, take);
        v->fill += (uint32_t)take;
        i += take;
        if (v->fill == HIL_SDL_B64CHUNK) hil_sdl_dump_flush(v);
    }
    return 0;
}

static int hil_sdl_dump(const char *name, uint32_t from_off)
{
    hil_sdl_dump_t v;
    memset(&v, 0, sizeof v);
    hil_sdl_sha256_init(&v.whole);
    hil_sdl_sha256_init(&v.range);
    v.from_off = from_off;
    uint32_t gen0 = hil_sdl_guard_gen(), size = 0;
    const char *why = "-";
    int r = hil_sdl_read(name, 10LL * 1000000, hil_sdl_dump_cb, &v, &size, &why);
    if (r == -2) { printf("SDL_TIMEOUT %s\n", name); return 1; }
    if (r != 0) { printf("SDL_INVALID %s %s\n", name, why); return 1; }
    if (hil_sdl_guard_gen() != gen0) { printf("SDL_INVALID %s guard\n", name); return 1; }
    hil_sdl_dump_flush(&v);
    uint8_t d1[32], d2[32];
    char h1[65], h2[65];
    hil_sdl_sha256_final(&v.whole, d1);
    hil_sdl_sha256_final(&v.range, d2);
    hil_sdl_hex(d1, 32, h1);
    hil_sdl_hex(d2, 32, h2);
    printf("SDL_DUMP_END %u %u %s %s\n", (unsigned)from_off, (unsigned)size, h1, h2);
    return 0;
}

/* ── H3 emitter ─────────────────────────────────────────────────────────── */
typedef struct {
    char     run[33];
    uint32_t k0, n, hz, pad;
} hil_sdl_emit_t;
static hil_sdl_emit_t hil_sdl_emit_cfg;

static void hil_sdl_emit_task(void *arg)
{
    (void)arg;
    hil_sdl_emit_t c = hil_sdl_emit_cfg;
    char pad[161];
    printf("SDL_BEGIN %s %u %u %u %u %lld\n", c.run, (unsigned)c.k0, (unsigned)c.n, (unsigned)c.hz, (unsigned)c.pad,
           (long long)esp_timer_get_time());
    TickType_t period = pdMS_TO_TICKS(1000 / c.hz);
    if (period == 0) period = 1;
    TickType_t last = xTaskGetTickCount();
    uint32_t k = c.k0;
    for (uint32_t i = 0; i < c.n; i++) {
        k = c.k0 + i;
        (void)evq_hil_pad(c.run, k, c.pad, pad, sizeof pad);
        ESP_LOGW("HILSDLOG", "%s %u %s", c.run, (unsigned)k, pad);   /* the real producer path */
        vTaskDelayUntil(&last, period);
    }
    printf("SDL_END %s %u %lld\n", c.run, (unsigned)k, (long long)esp_timer_get_time());
    hil_sdl_emit_busy = false;
    vTaskDelete(NULL);
}

static int hil_sdl_emit(int argc, char **argv)
{
    hil_sdl_emit_t c;
    memset(&c, 0, sizeof c);
    if (argc != 7 || strlen(argv[2]) == 0 || strlen(argv[2]) > 32 || !hil_sdl_u32(argv[3], &c.k0) ||
        !hil_sdl_u32(argv[4], &c.n) || !hil_sdl_u32(argv[5], &c.hz) || !hil_sdl_u32(argv[6], &c.pad) || c.n == 0 ||
        c.hz < 1 || c.hz > 50 || c.pad > 160) {
        printf("SDL_ERR usage: sdlog_emit <run> <k0> <n> <hz 1..50> <pad<=160>\n");
        return 1;
    }
    char probe[2];
    snprintf(c.run, sizeof c.run, "%s", argv[2]);
    if (evq_hil_pad(c.run, 0, 1, probe, sizeof probe) == 0) {
        printf("SDL_ERR bad run id\n");
        return 1;
    }
    if (hil_sdl_emit_busy) {
        printf("SDL_ERR busy\n");
        return 1;
    }
    hil_sdl_emit_busy = true;
    hil_sdl_emit_cfg = c;
    if (xTaskCreate(hil_sdl_emit_task, "sdl_emit", 3072, NULL, 1, NULL) != pdPASS) {
        hil_sdl_emit_busy = false;
        printf("SDL_ERR task\n");
        return 1;
    }
    return 0;
}

/* ── dispatcher ─────────────────────────────────────────────────────────── */
int evq_hil_sdlog_cmd(int argc, char **argv)
{
    const char *sub = argv[1];
    if (strcmp(sub, "sdlog_emit") == 0) return hil_sdl_emit(argc, argv);
    if (strcmp(sub, "sdlog_trace") == 0) {
        const char *a = argc >= 3 ? argv[2] : "";
        if (strcmp(a, "on") == 0) return hil_sdl_trace_on(false) == 0 ? 0 : 1;
        if (strcmp(a, "off") == 0) { hil_sdl_trace_off(); return 0; }
        if (strcmp(a, "drain") == 0) { hil_sdl_trace_drain(); return 0; }
        if (strcmp(a, "stat") == 0) { hil_sdl_trace_stat(); return 0; }
        if (strcmp(a, "autoarm") == 0) { hil_sdl_autoarm_set(); return 0; }
        if (strcmp(a, "probe") == 0 && argc >= 4) return hil_sdl_trace_probe((size_t)strtoul(argv[3], NULL, 0)) == 0 ? 0 : 1;
        printf("SDL_ERR usage: sdlog_trace <on|off|drain|stat|autoarm|probe <min_free>>\n");
        return 1;
    }
    bool inv = strcmp(sub, "sdlog_inv") == 0, dump = strcmp(sub, "sdlog_dump") == 0;
    if (!inv && !dump) return -1;
    uint32_t from = 0;
    if ((inv && argc != 2 && argc != 4) || (dump && argc != 3 && argc != 4) ||
        (argc == 4 && !hil_sdl_u32(argv[3], &from))) {
        printf("SDL_ERR usage: sdlog_inv [<name> <from_off>] | sdlog_dump <name> [<from_off>]\n");
        return 1;
    }
    if (hil_sdl_busy || hil_sdl_emit_busy) { printf("SDL_ERR busy\n"); return 1; }
    if (!hil_sdl_bufs()) { printf("SDL_ERR noalloc\n"); return 1; }
    hil_sdl_busy = true;
    uint32_t gen = 0;
    int rc = 1;
    if (hil_sdl_quiesce(&gen) == 0) {
        rc = inv ? hil_sdl_inv(argc == 4 ? argv[2] : NULL, from) : hil_sdl_dump(argv[2], from);
        hil_sdl_resume();                   /* every exit path after a successful pause */
    }
    hil_sdl_busy = false;
    return rc;
}
