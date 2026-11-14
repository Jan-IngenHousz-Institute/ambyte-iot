/* Host driver for components/diag: the PRODUCTION diag_reset.c, diag_crash.c,
 * diag_json.c and diag_log.c (hook, ring, rate limiter, renderer). Only the
 * four port facts (lock, clock, ISR flag, task id) are supplied here, as
 * pthread equivalents. Each scenario prints machine-readable lines that
 * tests/test_diag.py checks; internal invariants are asserted in C. */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_crash.h"
#include "diag_fmt.h"
#include "diag_log.h"
#include "diag_log_port.h"
#include "diag_reset.h"

/* ── port ──────────────────────────────────────────────────────────────── */
static pthread_mutex_t g_mux = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int64_t g_now_us;
static atomic_bool g_isr;
void diag_port_lock(void) { pthread_mutex_lock(&g_mux); }
void diag_port_unlock(void) { pthread_mutex_unlock(&g_mux); }
int64_t diag_port_uptime_us(void) { return atomic_load(&g_now_us); }
bool diag_port_in_isr(void) { return atomic_load(&g_isr); }
void *diag_port_task(void) { return (void *)pthread_self(); }

/* ── sinks ─────────────────────────────────────────────────────────────── */
static _Atomic unsigned g_forwarded;
static int sink(const char *fmt, va_list ap)
{
    char tmp[512];
    atomic_fetch_add(&g_forwarded, 1);
    return vsnprintf(tmp, sizeof tmp, fmt, ap);
}

/* Downstream fallback (diag_log_priv.h): defined outside the hook object. */
int diag_log_fallback_sink(const char *fmt, va_list ap);
int diag_log_fallback_sink(const char *fmt, va_list ap) { return sink(fmt, ap); }

static int logv(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = diag_log_vprintf(fmt, ap);
    va_end(ap);
    return r;
}
/* What ESP_LOGE expands to under log v1 without colours. */
#define LOGE(tag, fmt, ...) logv("E (%lu) %s: " fmt "\n", 1234UL, tag, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) logv("W (%lu) %s: " fmt "\n", 1234UL, tag, ##__VA_ARGS__)

static void reset_all(void)
{
    diag_log_reset_for_test();
    diag_log_set_next(sink);
    atomic_store(&g_now_us, 1000000);
    atomic_store(&g_isr, false);
    atomic_store(&g_forwarded, 0);
}

/* Seal + render + commit one record (store succeeded); prints it. */
static int drain_once(bool clock_valid, int64_t utc_ms, bool print)
{
    diag_log_batch_t b;
    if (!diag_log_seal(&b, atomic_load(&g_now_us))) return 0;
    char out[DIAG_LOG_RECORD_CAP + 1];
    diag_log_render_ctx_t ctx = {.measure_id = 42, .device = "AA:BB:CC:DD:EE:FF",
        .observed_utc_ms = utc_ms, .boot_count = 7, .now_uptime_us = atomic_load(&g_now_us),
        .clock_valid = clock_valid, .now_utc_ms = utc_ms};
    int n = diag_log_render(&b, &ctx, out, sizeof out);
    assert(n > 0 && n <= (int)DIAG_LOG_RECORD_CAP && (size_t)n == strlen(out));
    if (print) printf("RECORD %s\n", out);
    diag_log_commit(&b, atomic_load(&g_now_us));
    return 1;
}

/* ── scenarios ─────────────────────────────────────────────────────────── */

static void sc_reset(void)
{
    printf("{");
    for (int r = 0; r <= 17; r++) printf("%s\"%d\":\"%s\"", r ? "," : "", r, diag_reset_reason_name((esp_reset_reason_t)r));
    printf(",\"-1\":\"%s\"}\n", diag_reset_reason_name((esp_reset_reason_t)-1));
}

static void sc_crash(void)
{
    char out[DIAG_CRASH_JSON_CAP];
    diag_crash_t c;
    memset(&c, 0, sizeof c);                                   /* absent */
    assert(diag_crash_render_json(&c, out, sizeof out) > 0); printf("ABSENT %s\n", out);
    c.error = "no_partition";                                  /* absent with a reason */
    assert(diag_crash_render_json(&c, out, sizeof out) > 0); printf("ABSENT_ERR %s\n", out);
    assert(diag_crash_render_json(NULL, out, sizeof out) > 0); printf("NULL %s\n", out);

    memset(&c, 0, sizeof c);                                   /* present, full worst case */
    c.present = true; c.summary_valid = true; c.id = 0x0000a1b2c3d4e5f6ULL;
    c.first_seen_valid = true; c.first_seen_boot = 4294967295u;
    memcpy(c.app_elf_sha256, "0123456789abcdef", 17);
    strcpy(c.task, "\x01\x02\x03\x04\x05\x06\x07\x08\x0b\x0c\x0e\x0f\x10\x11\x12"); /* worst escaping */
    c.pc = 0x4200abcd; c.exc_cause = 29; c.exc_vaddr = 0xdeadbeef;
    c.bt_depth = 40;                                           /* clamped to 16 */
    for (int i = 0; i < 16; i++) c.backtrace[i] = 0x40370000u + (unsigned)i;
    c.bt_corrupted = true;
    int n = diag_crash_render_json(&c, out, sizeof out);
    assert(n > 0 && n < (int)DIAG_CRASH_JSON_CAP);
    printf("PRESENT_WORST %s\n", out);
    strcpy(c.task, "mqtt_task"); c.bt_depth = 3; c.bt_corrupted = false;
    assert(diag_crash_render_json(&c, out, sizeof out) > 0); printf("PRESENT %s\n", out);

    memset(&c, 0, sizeof c);                                   /* corrupt: CRC failed */
    c.present = true; c.id = 0x1234ULL << 32 | 0x9abcdef0u; c.first_seen_valid = true;
    c.first_seen_boot = 3; c.error = "ESP_ERR_INVALID_CRC";
    assert(diag_crash_render_json(&c, out, sizeof out) > 0); printf("CORRUPT %s\n", out);
    char small[16];
    assert(diag_crash_render_json(&c, small, sizeof small) == -1);  /* never a truncated object */
}

static void sc_parse(void)
{
    reset_all();
    LOGE("sd_card", "mount failed: %s", "ESP_ERR_TIMEOUT");
    logv("E something without the IDF prefix\n");
    char big[400];
    memset(big, 'x', sizeof big); big[sizeof big - 1] = '\0';
    LOGE("long", "%s", big);                                   /* truncated to 160 bytes */
    /* "—" (3 bytes) straddling byte 160 must be cut whole, not torn. */
    char dash[200];
    memset(dash, 'y', 159); memcpy(dash + 159, "\xe2\x80\x94tail", 8);
    LOGE("utf8", "%s", dash);
    LOGE("ctl", "quote\" back\\ tab\t bell\a bad\xff end");
    LOGW("warn", "not captured");
    logv("I (%lu) %s: info\n", 1UL, "x");
    assert(atomic_load(&g_forwarded) == 7);                    /* every line reaches the sinks */
    drain_once(false, 0, true);
}

static void sc_coalesce(void)
{
    reset_all();
    for (int i = 0; i < 100; i++) LOGE("loop", "same error %d", 5);
    diag_log_batch_t b;
    assert(diag_log_seal(&b, atomic_load(&g_now_us)) && b.n == 1);
    LOGE("loop", "same error %d", 5);                          /* sealed entry: new entry */
    LOGE("loop", "same error %d", 5);                          /* coalesces into the NEW one */
    diag_log_unseal();                                         /* store "failed" */
    atomic_fetch_add(&g_now_us, 1);
    drain_once(true, 1763100000000LL, true);                   /* 100 + 2, exactly once */
    atomic_fetch_add(&g_now_us, 61000000);
    LOGE("loop", "same error %d", 5);
    drain_once(true, 1763100061000LL, true);                   /* a fresh 1 */
    diag_log_stats_t st;
    diag_log_stats(&st);
    printf("STATS captured=%u repeats=%u records=%u\n", st.total_captured, st.total_repeats, st.total_records);
}

static void sc_ring_full(void)
{
    reset_all();
    for (int i = 0; i < 20; i++) LOGE("t", "line %d", i);      /* burst */
    LOGE("t", "line %d", 20);                                  /* no token: rate_limited */
    atomic_fetch_add(&g_now_us, 4 * DIAG_LOG_MIN_REFILL_US);   /* 4 more tokens */
    for (int i = 21; i < 25; i++) LOGE("t", "line %d", i);     /* ring now 24/24 */
    atomic_fetch_add(&g_now_us, DIAG_LOG_MIN_REFILL_US);       /* a token exists… */
    LOGE("t", "line %d", 99);                                  /* …but the ring is full */
    diag_log_stats_t st;
    diag_log_stats(&st);
    printf("STATS buffered=%u dropped=%u rate_limited=%u\n", st.buffered, st.dropped, st.rate_limited);
    atomic_fetch_add(&g_now_us, 1);
    drain_once(false, 0, true);
    /* The dropped line did not spend its token: one admission still possible. */
    LOGE("t", "after %d", 1);
    diag_log_stats(&st);
    printf("AFTER buffered=%u dropped=%u rate_limited=%u\n", st.buffered, st.dropped, st.rate_limited);
}

/* Downstream sink that logs from inside the hook — recursion. */
static int g_depth;
static int recursing_sink(const char *fmt, va_list ap)
{
    char tmp[256];
    atomic_fetch_add(&g_forwarded, 1);
    int r = vsnprintf(tmp, sizeof tmp, fmt, ap);
    if (g_depth++ == 0) LOGE("inner", "logged from a sink %d", 1);
    g_depth--;
    return r;
}

static void sc_recursion(void)
{
    reset_all();
    diag_log_set_next(recursing_sink);
    LOGE("outer", "first %d", 1);
    diag_log_stats_t st;
    diag_log_stats(&st);
    printf("STATS buffered=%u dropped=%u forwarded=%u\n", st.buffered, st.dropped, atomic_load(&g_forwarded));
    drain_once(false, 0, true);
    diag_log_set_next(sink);
    atomic_store(&g_isr, true);                                /* ISR: forwarded, dropped */
    LOGE("isr", "from isr %d", 1);
    atomic_store(&g_isr, false);
    diag_log_stats(&st);
    printf("ISR buffered=%u dropped=%u forwarded=%u\n", st.buffered, st.dropped, atomic_load(&g_forwarded));
}

static void sc_clock(void)
{
    reset_all();
    LOGE("t", "before the clock %d", 1);
    atomic_fetch_add(&g_now_us, 5000000);
    diag_log_note_clock_valid(atomic_load(&g_now_us));         /* RTC/SNTP made it valid */
    atomic_fetch_add(&g_now_us, 2000000);
    LOGE("t", "after the clock %d", 2);
    atomic_fetch_add(&g_now_us, 3000000);                      /* drain 3 s later */
    printf("NOW_US %lld\n", (long long)atomic_load(&g_now_us));
    drain_once(true, 1763100000000LL, true);
    LOGE("t", "drain too soon %d", 3);
    atomic_fetch_add(&g_now_us, 59000000);
    printf("SOON %d\n", drain_once(true, 1763100059000LL, false));   /* < 60 s: no record */
    atomic_fetch_add(&g_now_us, 1000000);
    printf("LATER %d\n", drain_once(true, 1763100060000LL, false));
}

static void sc_size(void)
{
    reset_all();
    /* 24 lines that need maximal escaping: far more than one record holds. */
    char worst[DIAG_LOG_MSG_MAX + 1];
    for (int i = 0; i < 24; i++) {
        memset(worst, '\x01', DIAG_LOG_MSG_MAX); worst[DIAG_LOG_MSG_MAX] = '\0';
        worst[0] = (char)('A' + i);
        if (i == 20) atomic_fetch_add(&g_now_us, 4 * DIAG_LOG_MIN_REFILL_US);
        logv("E (%lu) %s: %s\n", 1UL, "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01", worst);
    }
    diag_log_stats_t st;
    diag_log_stats(&st);
    assert(st.buffered == 24);
    int records = 0;
    for (int i = 0; i < 40; i++) {
        diag_log_stats(&st);
        if (st.buffered == 0) break;
        atomic_fetch_add(&g_now_us, DIAG_LOG_MIN_INTERVAL_US);
        records += drain_once(true, 1763100000000LL + i, true);
    }
    printf("RECORDS %d\n", records);
}

static void sc_bucket(void)
{
    reset_all();
    /* Dense random arrivals for 3 h; the ring is emptied after every line so
     * only the buckets decide. Admission times go to Python for the
     * sliding-window check. */
    unsigned seed = 12345;
    diag_log_stats_t st;
    unsigned prev = 0;
    for (int i = 0; i < 40000; i++) {
        seed = seed * 1103515245u + 12345u;
        atomic_fetch_add(&g_now_us, (int64_t)(seed >> 8) % 600000);   /* mean 0.3 s gap */
        if (atomic_load(&g_now_us) > 3LL * 3600 * 1000000 + 1000000) break;
        LOGE("b", "line %d", i);
        diag_log_stats(&st);
        if (st.total_captured != prev) {
            printf("ADMIT %lld\n", (long long)atomic_load(&g_now_us));
            prev = st.total_captured;
            diag_log_flush_for_test();   /* empty the ring, keep the buckets */
        }
    }
}

/* ── concurrency: producers on many threads + a drainer ────────────────── */
#define PRODUCERS 12
#define PER_THREAD 3000
static atomic_bool g_stop;

static void *producer(void *arg)
{
    const int id = (int)(intptr_t)arg;
    for (int i = 0; i < PER_THREAD; i++) {
        if (i % 3 == 0) LOGE("conc", "repeated by everyone");     /* coalescing under contention */
        else LOGE("conc", "thread %d line %d", id, i);
    }
    return NULL;
}

static void *drainer(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        atomic_fetch_add(&g_now_us, DIAG_LOG_MIN_INTERVAL_US);   /* buckets refill, gate opens */
        drain_once(true, 1763100000000LL, true);
    }
    return NULL;
}

static void sc_concurrent(void)
{
    reset_all();
    pthread_t p[PRODUCERS], d;
    pthread_create(&d, NULL, drainer, NULL);
    for (int i = 0; i < PRODUCERS; i++) pthread_create(&p[i], NULL, producer, (void *)(intptr_t)i);
    for (int i = 0; i < PRODUCERS; i++) pthread_join(p[i], NULL);
    atomic_store(&g_stop, true);
    pthread_join(d, NULL);
    for (int i = 0; i < 200; i++) {                             /* flush what is left */
        atomic_fetch_add(&g_now_us, DIAG_LOG_MIN_INTERVAL_US);
        drain_once(true, 1763100000000LL, true);
    }
    diag_log_stats_t st;
    diag_log_stats(&st);
    assert(st.buffered == 0 && st.dropped == 0 && st.rate_limited == 0);
    printf("TOTAL %d forwarded=%u\n", PRODUCERS * PER_THREAD, atomic_load(&g_forwarded));
}

/* ── formatter vs host libc ────────────────────────────────────────────── */
static void cmp(const char *kind, const char *fmt, ...)
{
    char ours[256], libc[256];
    uint32_t unsupported = 0;
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    size_t n = diag_vsnformat(ours, sizeof ours, fmt, a, &unsupported);
    vsnprintf(libc, sizeof libc, fmt, b);
    va_end(b);
    va_end(a);
    assert(n == strlen(ours));
    printf("%s\t%s\t%s\t%s\t%u\n", kind, fmt, ours, libc, unsupported);
}

static void ours_only(const char *fmt, ...)
{
    char out[64];
    uint32_t unsupported = 0;
    va_list a;
    va_start(a, fmt);
    size_t n = diag_vsnformat(out, sizeof out, fmt, a, &unsupported);
    va_end(a);
    assert(n == strlen(out));
    printf("ODD\t%s\t%s\t%u\n", fmt, out, unsupported);
}

static void sc_fmt(void)
{
    /* Exact against libc: integers, strings, chars, widths, flags, lengths. */
    cmp("INT", "E (%lu) %s: plain", 4294967295UL, "tag");
    cmp("INT", "%d|%i|%u|%x|%X|%o", -42, 7, 3000000000U, 0xbeefU, 0xbeefU, 8U);
    cmp("INT", "%5d|%-5d|%05d|%+d|% d|%.3d|%.0d|%8.3d", 42, 42, -42, 42, 42, 7, 0, -7);
    cmp("INT", "%#x|%#X|%#o|%#x|%#o", 255U, 255U, 8U, 0U, 0U);
    cmp("INT", "%hhd|%hhu|%hd|%hu", 300, 300, 70000, 70000);
    cmp("INT", "%ld|%lu|%lld|%llu|%llx", -1L, 1UL, -9223372036854775807LL - 1, 18446744073709551615ULL,
        0x123456789abcdefULL);
    cmp("INT", "%zu|%zd|%jd|%ju|%td", (size_t)123, (ptrdiff_t)-5, (intmax_t)-6, (uintmax_t)7, (ptrdiff_t)8);
    cmp("INT", "%s|%10s|%-10s|%.3s|%*s|%-*s|%.*s", "abc", "abc", "abc", "abcdef", 6, "ab", 6, "ab", 2, "xyz");
    cmp("INT", "%c|%3c|%-3c|%%|100%%", 'a', 'b', 'c');
    cmp("INT", "%*d|%-*d", -6, 1, 4, 2);
    /* Floats: libc output beside ours for a numeric comparison in Python. */
    cmp("FLT", "%f", 3.14159265);
    cmp("FLT", "%.2f", -0.005);
    cmp("FLT", "%.3f", 2.0005);
    cmp("FLT", "%10.4f", 1234.5678);
    cmp("FLT", "%-10.1f|", 9.96);
    cmp("FLT", "%08.2f", -3.5);
    cmp("FLT", "%+.1f", 2.25);
    cmp("FLT", "%.0f", 7.6);
    cmp("FLT", "%f", 1e15);
    cmp("FLT", "%.1f", 123456789012.34);
    cmp("FLT", "%e", 12345.678);
    cmp("FLT", "%.3E", 0.000123456);
    cmp("FLT", "%e", 0.0);
    cmp("FLT", "%e", 9.9999999);
    cmp("FLT", "%g", 0.0001);
    cmp("FLT", "%g", 123456789.0);
    cmp("FLT", "%g", 100.0);
    cmp("FLT", "%.3g", 3.14159);
    cmp("FLT", "%G", 1e-10);
    cmp("FLT", "%#g", 1.5);
    cmp("FLT", "%f", 1e300);
    cmp("FLT", "%Lf", (long double)2.5);
    cmp("FLT", "%.2f°C", 21.345);
    cmp("SPECIAL", "%f|%F|%e|%g", 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 1.0 / 0.0);
    /* Not supported: counted, consumed, visible. */
    int n_target = 0;
    ours_only("a=%a b=%d", 1.5, 7);
    ours_only("n=%n then %d", &n_target, 9);
    ours_only("w=%ls %lc end %d", (void *)0, 65, 3);
    ours_only("bad %q %d", 1, 2);
    ours_only("trailing %");
    assert(n_target == 0);                                     /* %n never writes */
}

static size_t fmt_into(char *out, size_t cap, uint32_t *u, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    size_t n = diag_vsnformat(out, cap, fmt, a, u);
    va_end(a);
    return n;
}

static void sc_fmt_trunc(void)
{
    char out[8];
    uint32_t u = 0;
    size_t n = fmt_into(out, sizeof out, &u, "%s-%d", "abcdefghij", 12345);
    assert(n == 7 && strcmp(out, "abcdefg") == 0);
    n = fmt_into(out, 1, &u, "%d", 5);
    assert(n == 0 && out[0] == '\0');
    n = fmt_into(out, sizeof out, &u, "%f", 1e300);   /* e-form fallback, cut */
    assert(n == 7 && u == 0);
    puts("TRUNC ok");
}

/* An E line with a float through the real hook: formatted without newlib,
 * and %n / %a lines counted in unsupported_fmt. */
static void sc_hook_fmt(void)
{
    reset_all();
    LOGE("bme280", "temperature %.2f out of range (%d)", 85.125, 3);
    int written_by_sink = -1;                                  /* the libc sink honours %n; diag never does */
    LOGE("weird", "count=%n done", &written_by_sink);
    LOGE("weird", "hex float %a", 0.5);
    diag_log_stats_t st;
    diag_log_stats(&st);
    printf("STATS unsupported=%u buffered=%u\n", st.unsupported_fmt, st.buffered);
    drain_once(false, 0, true);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *s = argv[1];
    if (!strcmp(s, "reset")) sc_reset();
    else if (!strcmp(s, "crash")) sc_crash();
    else if (!strcmp(s, "parse")) sc_parse();
    else if (!strcmp(s, "coalesce")) sc_coalesce();
    else if (!strcmp(s, "ring_full")) sc_ring_full();
    else if (!strcmp(s, "recursion")) sc_recursion();
    else if (!strcmp(s, "clock")) sc_clock();
    else if (!strcmp(s, "size")) sc_size();
    else if (!strcmp(s, "bucket")) sc_bucket();
    else if (!strcmp(s, "concurrent")) sc_concurrent();
    else if (!strcmp(s, "fmt")) sc_fmt();
    else if (!strcmp(s, "fmt_trunc")) sc_fmt_trunc();
    else if (!strcmp(s, "hook_fmt")) sc_hook_fmt();
    else return 2;
    return 0;
}
