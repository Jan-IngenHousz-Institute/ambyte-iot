/* sd_logger verification trace (see evq_hil_sdl.h, docs/sdlog-hil-trace.md).
 * Verification build only; also compiled on the host (EVQ_HIL_HOST) by the
 * sd_logger harness so the replay model is proven against byte-exact files. */
#include "evq_hil_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef EVQ_HIL_HOST
#include <pthread.h>
#include <time.h>
static pthread_mutex_t slt_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t slt_drain_lk = PTHREAD_MUTEX_INITIALIZER;
#define SLT_LOCK()   pthread_mutex_lock(&slt_lk)
#define SLT_UNLOCK() pthread_mutex_unlock(&slt_lk)
static bool slt_drain_take(int wait_ms) { (void)wait_ms; return pthread_mutex_lock(&slt_drain_lk) == 0; }
static void slt_drain_give(void) { pthread_mutex_unlock(&slt_drain_lk); }
static int64_t slt_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void *slt_alloc(size_t n) { return malloc(n); }
static void slt_flush_out(void) { fflush(stdout); }
#define SLT_RTC_NOINIT
#else
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static portMUX_TYPE slt_mux = portMUX_INITIALIZER_UNLOCKED;
#define SLT_LOCK()   portENTER_CRITICAL_SAFE(&slt_mux)
#define SLT_UNLOCK() portEXIT_CRITICAL_SAFE(&slt_mux)
static StaticSemaphore_t slt_drain_buf;
static SemaphoreHandle_t slt_drain_mtx;
static bool slt_drain_take(int wait_ms)
{
    if (slt_drain_mtx == NULL) slt_drain_mtx = xSemaphoreCreateMutexStatic(&slt_drain_buf);
    return xSemaphoreTake(slt_drain_mtx, wait_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}
static void slt_drain_give(void) { xSemaphoreGive(slt_drain_mtx); }
static int64_t slt_now_us(void) { return esp_timer_get_time(); }
static void *slt_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void slt_flush_out(void)
{
    /* Give the USB-serial-JTAG FIFO time to empty: evidence printed right before
     * a ROM reset is otherwise lost with the CPU (bounded, ≤ 500 ms). */
    fflush(stdout);
    for (int i = 0; i < 25; i++) esp_rom_delay_us(20000);
}
#define SLT_RTC_NOINIT RTC_NOINIT_ATTR
#endif

/* ── entry layout ──────────────────────────────────────────────────────────
 * Every entry is 8-byte aligned (total is a multiple of 8, cap too), so the
 * space left before the buffer end is always 0 or ≥ 8 — room for a wrap marker. */
typedef struct {
    uint32_t total;          /* header + payload, rounded up to 8 */
    uint8_t  kind;           /* 0 = P, 0x10 = POP, HIL_SDL_* writer kinds, 0xFF = wrap marker */
    uint8_t  flag;           /* P: bit0 pushed, bit1 syn */
    uint16_t plen;
    uint64_t seq;
    int64_t  us;
    uint32_t a, b, c;
} slt_ent_t;

#define K_P    0x00
#define K_POP  0x10
#define K_WRAP 0xFF

static uint8_t *slt_buf;
static size_t   slt_head, slt_tail, slt_used;
static uint64_t slt_next_seq = 1, slt_total, slt_lost, slt_drained_upto;
static bool     slt_on, slt_wm_armed = true, slt_wm_due;

/* ── helpers: sha256 / base64 / hex (portable, identical on host and device) ── */
static const uint32_t slt_k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void slt_sha_block(hil_sdl_sha_ctx_t *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + slt_k256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void hil_sdl_sha256_init(void *ctx)
{
    static const uint32_t slt_iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    hil_sdl_sha_ctx_t *c = ctx;
    memcpy(c->h, slt_iv, sizeof slt_iv);
    c->len = 0;
    c->fill = 0;
}

void hil_sdl_sha256_update(void *ctx, const void *data, size_t n)
{
    hil_sdl_sha_ctx_t *c = ctx;
    const uint8_t *p = data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->fill;
        if (take > n) take = n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        n -= take;
        if (c->fill == 64) { slt_sha_block(c, c->buf); c->fill = 0; }
    }
}

void hil_sdl_sha256_final(void *ctx, uint8_t out[32])
{
    hil_sdl_sha_ctx_t *c = ctx;
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    hil_sdl_sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->fill != 56) hil_sdl_sha256_update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    hil_sdl_sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void hil_sdl_sha256(const void *p, size_t n, uint8_t out[32])
{
    hil_sdl_sha_ctx_t c;
    hil_sdl_sha256_init(&c);
    hil_sdl_sha256_update(&c, p, n);
    hil_sdl_sha256_final(&c, out);
}

size_t hil_sdl_b64(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char slt_b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 4 * ((n + 2) / 3);
    if (cap < need + 1) { if (cap) out[0] = '\0'; return 0; }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = slt_b64tab[(v >> 18) & 63];
        out[o++] = slt_b64tab[(v >> 12) & 63];
        out[o++] = i + 1 < n ? slt_b64tab[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? slt_b64tab[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

void hil_sdl_hex(const uint8_t *in, size_t n, char *out)
{
    static const char slt_hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = slt_hexd[in[i] >> 4]; out[2 * i + 1] = slt_hexd[in[i] & 15]; }
    out[2 * n] = '\0';
}

/* ── recording ───────────────────────────────────────────────────────────── */

/* Caller holds SLT_LOCK. Returns the payload pointer or NULL (entry lost, seq consumed). */
static uint8_t *slt_reserve(uint8_t kind, uint8_t flag, const void *payload, uint16_t plen, uint32_t a, uint32_t b,
                            uint32_t c)
{
    uint64_t seq = slt_next_seq++;
    size_t total = (sizeof(slt_ent_t) + plen + 7u) & ~(size_t)7u;
    size_t pad = (HIL_SDL_CAP_BYTES - slt_head < total) ? HIL_SDL_CAP_BYTES - slt_head : 0;
    if (slt_buf == NULL || slt_used + pad + total > HIL_SDL_CAP_BYTES) {
        slt_lost++;
        return NULL;
    }
    if (pad) {
        slt_ent_t *w = (slt_ent_t *)(slt_buf + slt_head);
        w->total = (uint32_t)pad;
        w->kind = K_WRAP;
        slt_used += pad;
        slt_head = 0;
    }
    slt_ent_t *e = (slt_ent_t *)(slt_buf + slt_head);
    e->total = (uint32_t)total;
    e->kind = kind;
    e->flag = flag;
    e->plen = plen;
    e->seq = seq;
    e->us = slt_now_us();
    e->a = a;
    e->b = b;
    e->c = c;
    if (plen) memcpy((uint8_t *)(e + 1), payload, plen);
    slt_head = (slt_head + total) % HIL_SDL_CAP_BYTES;
    slt_used += total;
    slt_total++;
    if (slt_wm_armed && slt_used * 4 >= HIL_SDL_CAP_BYTES) { slt_wm_armed = false; slt_wm_due = true; }
    return (uint8_t *)(e + 1);
}

static bool slt_syn_record(const uint8_t *p, size_t n)
{
    /* The emitter's tag as the IDF v1 format renders it ("W (<ms>) HILSDLOG: ...").
     * Contract H2 says "contains `HILSDLOG `"; the rendered tag is followed by ':'
     * (disclosed interpretation; the host recomputes syn from the bytes anyway). */
    static const char slt_syntag[] = " HILSDLOG: ";
    for (size_t i = 0; i + sizeof slt_syntag - 1 <= n; i++) if (memcmp(p + i, slt_syntag, sizeof slt_syntag - 1) == 0) return true;
    return false;
}

void hil_sdl_push(const uint8_t *rec, size_t n, bool pushed)
{
    if (!slt_on || n > 0xFFFF) return;
    uint8_t flag = (uint8_t)((pushed ? 1 : 0) | (slt_syn_record(rec, n) ? 2 : 0));
    SLT_LOCK();
    (void)slt_reserve(K_P, flag, rec, (uint16_t)n, 0, 0, 0);
    SLT_UNLOCK();
}

void hil_sdl_pop(size_t n)
{
    if (!slt_on || n == 0) return;
    SLT_LOCK();
    (void)slt_reserve(K_POP, 0, NULL, 0, (uint32_t)n, 0, 0);
    SLT_UNLOCK();
}

void hil_sdl_after_push(void)
{
    if (!slt_wm_due) return;
    size_t used;
    SLT_LOCK();
    bool due = slt_wm_due;
    slt_wm_due = false;
    used = slt_used;
    SLT_UNLOCK();
    if (due) printf("SLT_WM %u\n", (unsigned)used);
}

void hil_sdl_ev(int kind, uint32_t a, uint32_t b, uint32_t c, const char *name)
{
    if (!slt_on) return;
    uint16_t plen = name ? (uint16_t)strnlen(name, 63) : 0;
    SLT_LOCK();
    (void)slt_reserve((uint8_t)kind, 0, name, plen, a, b, c);
    SLT_UNLOCK();
}

/* ── control ─────────────────────────────────────────────────────────────── */

int hil_sdl_trace_on(bool autoarm)
{
    if (slt_buf == NULL) slt_buf = slt_alloc(HIL_SDL_CAP_BYTES);
    if (slt_buf == NULL) {
        printf("SLT_ERR noalloc\n");
        return -1;
    }
    SLT_LOCK();
    slt_on = true;
    uint64_t nx = slt_next_seq;
    SLT_UNLOCK();
    printf("SLT_ON %u %llu%s\n", (unsigned)HIL_SDL_CAP_BYTES, (unsigned long long)nx, autoarm ? " autoarm" : "");
    return 0;
}

void hil_sdl_trace_off(void)
{
    SLT_LOCK();
    slt_on = false;
    uint64_t nx = slt_next_seq;
    SLT_UNLOCK();
    printf("SLT_OFF %llu\n", (unsigned long long)nx);
}

void hil_sdl_trace_stat(void)
{
    SLT_LOCK();
    bool on = slt_on;
    uint64_t nx = slt_next_seq, lost = slt_lost;
    size_t used = slt_used;
    SLT_UNLOCK();
    printf("SLT_STAT %d %llu %llu %u %u\n", on ? 1 : 0, (unsigned long long)nx, (unsigned long long)lost,
           (unsigned)used, (unsigned)HIL_SDL_CAP_BYTES);
}

static const char *slt_ev_name(uint8_t k)
{
    switch (k) {
    case K_P: return "P";
    case K_POP: return "POP";
    case HIL_SDL_WR: return "WR";
    case HIL_SDL_COMMIT: return "COMMIT";
    case HIL_SDL_RB: return "RB";
    case HIL_SDL_ROT: return "ROT";
    case HIL_SDL_OPEN: return "OPEN";
    case HIL_SDL_CLOSE: return "CLOSE";
    case HIL_SDL_DROP: return "DROP";
    case HIL_SDL_QUI: return "QUI";
    default: return "?";
    }
}

static void slt_print_ent(const slt_ent_t *e)
{
    static char slt_pb64[4 * ((256 + 2) / 3) + 8];  /* records are ≤ 256 B (F-L0) */
    static char slt_phex[65];
    const uint8_t *pl = (const uint8_t *)(e + 1);
    switch (e->kind) {
    case K_P: {
        uint8_t d[32];
        hil_sdl_sha256(pl, e->plen, d);
        hil_sdl_hex(d, 32, slt_phex);
        (void)hil_sdl_b64(pl, e->plen, slt_pb64, sizeof slt_pb64);
        printf("SLT_E P %llu %lld %u %s %s %d %s\n", (unsigned long long)e->seq, (long long)e->us, (unsigned)e->plen, slt_phex,
               (e->flag & 1) ? "pushed" : "dropped", (e->flag & 2) ? 1 : 0, slt_pb64);
        break;
    }
    case K_POP:
        printf("SLT_E POP %llu %lld %u\n", (unsigned long long)e->seq, (long long)e->us, (unsigned)e->a);
        break;
    case HIL_SDL_WR:
        printf("SLT_E WR %llu %lld %u %u %u\n", (unsigned long long)e->seq, (long long)e->us, (unsigned)e->a,
               (unsigned)e->b, (unsigned)e->c);
        break;
    case HIL_SDL_COMMIT:
        printf("SLT_E COMMIT %llu %lld %u\n", (unsigned long long)e->seq, (long long)e->us, (unsigned)e->a);
        break;
    case HIL_SDL_RB:
        printf("SLT_E RB %llu %lld %u %u %s\n", (unsigned long long)e->seq, (long long)e->us, (unsigned)e->a,
               (unsigned)e->b, e->c == HIL_SDL_RB_OK ? "ok" : e->c == HIL_SDL_RB_QUAR_TRUNC ? "quar_trunc" : "quar_fsync");
        break;
    case HIL_SDL_ROT:
        if (e->a == HIL_SDL_ROT_FAIL_RENAME) {
            /* step=<source index>: which rename of the 4→5..0→1 chain failed (the
             * replay must know which renames before it were applied). */
            printf("SLT_E ROT %llu %lld fail rename %u step=%u\n", (unsigned long long)e->seq, (long long)e->us,
                   (unsigned)e->b, (unsigned)e->c);
        } else {
            printf("SLT_E ROT %llu %lld %s %s %u\n", (unsigned long long)e->seq, (long long)e->us,
                   e->a == HIL_SDL_ROT_OK ? "ok" : "fail", e->a == HIL_SDL_ROT_FAIL_REMOVE ? "remove" : "-",
                   (unsigned)e->b);
        }
        break;
    case HIL_SDL_OPEN: {
        char nm[64];
        size_t n = e->plen < sizeof nm - 1 ? e->plen : sizeof nm - 1;
        memcpy(nm, pl, n);
        nm[n] = '\0';
        printf("SLT_E OPEN %llu %lld %s %u %u\n", (unsigned long long)e->seq, (long long)e->us, nm, (unsigned)e->a,
               (unsigned)e->b);
        break;
    }
    case HIL_SDL_CLOSE:
        printf("SLT_E CLOSE %llu %lld %s\n", (unsigned long long)e->seq, (long long)e->us,
               e->a == HIL_SDL_CLOSE_OK ? "ok" : e->a == HIL_SDL_CLOSE_ERR ? "err" : "abandon");
        break;
    case HIL_SDL_DROP:
        printf("SLT_E DROP %llu %lld %s %u\n", (unsigned long long)e->seq, (long long)e->us,
               e->a == HIL_SDL_DROP_ROTATE_BLOCKED ? "rotate_blocked" : "unavailable", (unsigned)e->b);
        break;
    case HIL_SDL_QUI:
        printf("SLT_E QUI %llu %lld %s\n", (unsigned long long)e->seq, (long long)e->us,
               e->a == HIL_SDL_QUI_PAUSED ? "paused" : e->a == HIL_SDL_QUI_RESUMED ? "resumed"
               : e->a == HIL_SDL_QUI_TIMEOUT ? "timeout" : "refused");
        break;
    default:
        printf("SLT_E %s %llu %lld\n", slt_ev_name(e->kind), (unsigned long long)e->seq, (long long)e->us);
        break;
    }
}

/* Entries in [tail, snap_head) are never touched by producers (they only write
 * at head and stop at tail), so they are printed WITHOUT the lock held —
 * producers keep recording while a drain streams to the console. */
static void slt_drain_out(void)
{
    SLT_LOCK();
    size_t tail = slt_tail, used_snap = slt_used;
    uint64_t first = slt_drained_upto + 1, last = slt_next_seq - 1;
    SLT_UNLOCK();
    printf("SLT_DRAIN %llu %llu\n", (unsigned long long)first, (unsigned long long)last);
    size_t pos = tail, consumed = 0;
    uint64_t last_printed = slt_drained_upto;
    while (consumed < used_snap && slt_buf != NULL) {
        const slt_ent_t *e = (const slt_ent_t *)(slt_buf + pos);
        if (e->kind != K_WRAP) {
            slt_print_ent(e);
            last_printed = e->seq;
        }
        consumed += e->total;
        pos = (pos + e->total) % HIL_SDL_CAP_BYTES;
    }
    SLT_LOCK();
    slt_tail = pos;
    slt_used -= consumed;
    slt_wm_armed = true;
    slt_drained_upto = last;
    uint64_t total = slt_total, lost = slt_lost;
    SLT_UNLOCK();
    (void)last_printed;
    printf("SLT_HDR %llu %llu %llu %llu %u\n", (unsigned long long)total, (unsigned long long)lost,
           (unsigned long long)first, (unsigned long long)last, (unsigned)used_snap);
}

void hil_sdl_trace_drain(void)
{
    if (!slt_drain_take(-1)) return;
    slt_drain_out();
    slt_drain_give();
}

/* ── H8a witness ────────────────────────────────────────────────────────── */
void hil_sdl_reset_witness(const char *path, uint32_t off_before, const void *chunk, size_t n, size_t half)
{
    static char slt_wb64[4 * ((1024 + 2) / 3) + 8];
    char hex[65];
    uint8_t d[32];
    if (n > 1024) n = 1024;                       /* sd_logger chunks are ≤ 512 B */
    hil_sdl_sha256(chunk, n, d);
    hil_sdl_hex(d, 32, hex);
    (void)hil_sdl_b64(chunk, n, slt_wb64, sizeof slt_wb64);
    printf("SLT_RESET_WITNESS %s %u %u %u %s %s\n", path ? path : "-", (unsigned)off_before, (unsigned)n,
           (unsigned)half, hex, slt_wb64);
    if (slt_drain_take(500)) {
        slt_drain_out();
        slt_drain_give();
    } else {
        printf("SLT_ERR drain_busy\n");          /* the case is void: no complete witness */
    }
    slt_flush_out();
}

/* ── H8b auto-arm (RTC one-shot) ───────────────────────────────────────── */
#define SLT_ARM_MAGIC 0x534C5441u   /* 'SLTA' */
static SLT_RTC_NOINIT struct { uint32_t magic, inv; } slt_autoarm;

void hil_sdl_autoarm_set(void)
{
    slt_autoarm.magic = SLT_ARM_MAGIC;
    slt_autoarm.inv = ~SLT_ARM_MAGIC;
    printf("SLT_AUTOARM set\n");
}

#ifdef EVQ_HIL_HOST
bool hil_sdl_host_power_on;
static bool slt_power_on(void) { return hil_sdl_host_power_on; }
#else
#include "esp_system.h"
static bool slt_power_on(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    return r == ESP_RST_POWERON || r == ESP_RST_UNKNOWN;   /* RTC contents meaningless */
}
#endif

void hil_sdl_boot_hook(void)
{
    bool armed = !slt_power_on() && slt_autoarm.magic == SLT_ARM_MAGIC && slt_autoarm.inv == ~SLT_ARM_MAGIC;
    slt_autoarm.magic = 0;
    slt_autoarm.inv = 0;
    if (armed) (void)hil_sdl_trace_on(true);
}

/* ── H7 guard interlock ─────────────────────────────────────────────────── */
static volatile uint32_t slt_guard_gen;
static volatile bool     slt_guard_parked;

void hil_sdl_guard_note(bool parked)
{
    slt_guard_parked = parked;
    slt_guard_gen++;
}
uint32_t hil_sdl_guard_gen(void) { return slt_guard_gen; }
bool hil_sdl_guard_parked(void) { return slt_guard_parked; }
