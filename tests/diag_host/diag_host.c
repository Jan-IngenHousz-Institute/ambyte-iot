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
    else return 2;
    return 0;
}
