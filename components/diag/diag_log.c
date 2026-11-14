/*
 * diag_log.c — capture ring, rate limiter and record renderer for
 * ambyte.log/1. The contract and the hook-chain order are documented in
 * include/diag_log.h; this file keeps the WHY of each mechanism at its
 * definition.
 *
 * Everything the hook touches is static and fixed-size: ~4.8 KiB of .bss
 * (24 × ~200 B lines), no heap, no locks other than g_diag_log_mux.
 */
#include "diag_log.h"

#include <stdio.h>
#include <string.h>

#include "diag_json.h"
#include "diag_log_port.h"

#ifndef DIAG_LOG_HOST
portMUX_TYPE g_diag_log_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

typedef struct {
    int64_t  uptime_us;                  /* first occurrence (esp_timer) */
    uint32_t repeat;                     /* occurrences coalesced into this entry, ≥ 1 */
    char     level;
    char     tag[DIAG_LOG_TAG_MAX];
    char     msg[DIAG_LOG_MSG_MAX + 1];
} diag_line_t;

typedef struct {
    uint32_t tokens;
    int64_t  last_us;                    /* virtual time of the last credited token */
    bool     init;
} diag_bucket_t;

/* ── state (all guarded by the port lock unless noted) ─────────────────── */
static diag_line_t   s_ring[DIAG_LOG_RING_LINES];
static uint32_t      s_tail;             /* oldest entry; written only by the drainer */
static uint32_t      s_count;
/* The oldest s_sealed entries belong to the record being built: producers
 * may neither coalesce into them nor reuse their slots. This is what makes
 * every occurrence land in exactly one record. */
static uint32_t      s_sealed;
static uint32_t      s_dropped;          /* not yet reported */
static uint32_t      s_rate_limited;     /* not yet reported */
static uint32_t      s_total_captured, s_total_repeats, s_total_records;
static diag_bucket_t s_min_bucket, s_hour_bucket;
static bool          s_have_record;
static int64_t       s_last_record_us;
/* First uptime at which the wall clock was known valid; -1 = not yet. Set
 * from task context only (app_main, the SNTP callback, the drainer) but by
 * several tasks, and a 64-bit store is two words on Xtensa: lock-guarded. */
static int64_t       s_clock_valid_since_us = -1;

/* Recursion guard WITHOUT thread-local storage: the tasks currently inside
 * the hook. A task found here again is re-entering (a log emitted by a sink
 * downstream of us while we handle its line). More concurrent loggers than
 * slots also drop+count rather than wait. */
#define DIAG_LOG_ACTIVE_MAX 8U
static void *s_active[DIAG_LOG_ACTIVE_MAX];

static diag_log_vprintf_fn s_next;

static inline void sat_inc(uint32_t *v) { if (*v != UINT32_MAX) (*v)++; }

/* ── rate limiting ─────────────────────────────────────────────────────── */

/* Lazy refill on the bucket's own virtual time grid. A full bucket's clock is
 * pinned to `now`, so credit can never pile up beyond `burst`; this is what
 * bounds any window W to burst + floor(W / period) admissions. */
static void bucket_refill(diag_bucket_t *b, uint32_t burst, int64_t period_us, int64_t now)
{
    if (!b->init) {
        b->tokens = burst;
        b->last_us = now;
        b->init = true;
        return;
    }
    if (now <= b->last_us) return;
    if (b->tokens >= burst) {
        b->last_us = now;
        return;
    }
    int64_t add = (now - b->last_us) / period_us;
    if (add <= 0) return;
    if ((uint64_t)b->tokens + (uint64_t)add >= burst) {
        b->tokens = burst;
        b->last_us = now;
    } else {
        b->tokens += (uint32_t)add;
        b->last_us += add * period_us;
    }
}

/* ── the hook ──────────────────────────────────────────────────────────── */

/* IDF log v1 (CONFIG_LOG_VERSION_1, colours off in this tree) hands the sink
 * LOG_FORMAT(E, fmt) = "E (%lu) %s: <fmt>\n". Matching "E " on the FORMAT
 * string decides before any formatting work; an optional CSI colour escape is
 * skipped so a CONFIG_LOG_COLORS build behaves the same. */
static bool level_is_error(const char *fmt)
{
    const char *p = fmt;
    if (p == NULL) return false;
    if (*p == '\033') {
        p++;
        if (*p == '[') {
            p++;
            while (*p && !(*p >= '@' && *p <= '~')) p++;
            if (*p) p++;
        }
    }
    return p[0] == 'E' && p[1] == ' ';
}

typedef enum { GUARD_OK, GUARD_RECURSION, GUARD_FULL } guard_t;

static guard_t guard_enter(void *me)
{
    guard_t g = GUARD_FULL;
    diag_port_lock();
    int free_slot = -1;
    for (unsigned i = 0; i < DIAG_LOG_ACTIVE_MAX; i++) {
        if (s_active[i] == me) { g = GUARD_RECURSION; break; }
        if (s_active[i] == NULL && free_slot < 0) free_slot = (int)i;
    }
    if (g != GUARD_RECURSION && free_slot >= 0) {
        s_active[free_slot] = me;
        g = GUARD_OK;
    }
    if (g != GUARD_OK) sat_inc(&s_dropped);
    diag_port_unlock();
    return g;
}

static void guard_exit(void *me)
{
    diag_port_lock();
    for (unsigned i = 0; i < DIAG_LOG_ACTIVE_MAX; i++) {
        if (s_active[i] == me) { s_active[i] = NULL; break; }
    }
    diag_port_unlock();
}

/* Strip CSI colour escapes in place (a coloured build embeds them). */
static size_t strip_ansi(char *s, size_t len)
{
    size_t w = 0;
    for (size_t r = 0; r < len; r++) {
        if (s[r] == '\033') {
            r++;
            if (r < len && s[r] == '[') {
                r++;
                while (r < len && !(s[r] >= '@' && s[r] <= '~')) r++;
            }
            continue;
        }
        s[w++] = s[r];
    }
    s[w] = '\0';
    return w;
}

/* Admit one parsed line. The only code that runs under the lock on the
 * logging task: compare with the newest entry, refill two buckets, copy. */
static void admit(int64_t now, const char *tag, const char *msg)
{
    diag_port_lock();
    if (s_count > s_sealed) {
        diag_line_t *nw = &s_ring[(s_tail + s_count - 1U) % DIAG_LOG_RING_LINES];
        if (nw->level == 'E' && strcmp(nw->tag, tag) == 0 && strcmp(nw->msg, msg) == 0) {
            sat_inc(&nw->repeat);
            sat_inc(&s_total_repeats);
            diag_port_unlock();
            return;
        }
    }
    if (s_count >= DIAG_LOG_RING_LINES) {
        /* Full before spending a token: a line we cannot keep must not eat
         * the budget of the lines that come after the drain. */
        sat_inc(&s_dropped);
        diag_port_unlock();
        return;
    }
    bucket_refill(&s_min_bucket, DIAG_LOG_MIN_BURST, DIAG_LOG_MIN_REFILL_US, now);
    bucket_refill(&s_hour_bucket, DIAG_LOG_HOUR_BURST, DIAG_LOG_HOUR_REFILL_US, now);
    if (s_min_bucket.tokens == 0 || s_hour_bucket.tokens == 0) {
        sat_inc(&s_rate_limited);
        diag_port_unlock();
        return;
    }
    s_min_bucket.tokens--;
    s_hour_bucket.tokens--;
    diag_line_t *e = &s_ring[(s_tail + s_count) % DIAG_LOG_RING_LINES];
    e->uptime_us = now;
    e->repeat = 1;
    e->level = 'E';
    strncpy(e->tag, tag, sizeof e->tag - 1);
    e->tag[sizeof e->tag - 1] = '\0';
    strncpy(e->msg, msg, sizeof e->msg - 1);
    e->msg[sizeof e->msg - 1] = '\0';
    s_count++;
    sat_inc(&s_total_captured);
    diag_port_unlock();
}

/* Format + parse + admit. Kept out of line so its ~260 B buffer lives only
 * AFTER the downstream sinks (sd_logger + console) have returned: the hook
 * adds its formatting frame next to theirs, never on top of them. */
static __attribute__((noinline)) void capture(const char *fmt, va_list ap)
{
    const int64_t now = diag_port_uptime_us();
    char buf[DIAG_LOG_TAG_MAX + DIAG_LOG_MSG_MAX + 72];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) {
        diag_port_lock();
        sat_inc(&s_dropped);
        diag_port_unlock();
        return;
    }
    size_t len = strip_ansi(buf, strnlen(buf, sizeof buf - 1));
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';

    /* "E (<ticks>) <tag>: <message>"; anything else keeps the whole line as
     * the message with an empty tag rather than being lost. */
    char tag[DIAG_LOG_TAG_MAX] = "";
    const char *msg = buf;
    if (buf[0] == 'E' && buf[1] == ' ' && buf[2] == '(') {
        const char *close = strstr(buf + 3, ") ");
        if (close != NULL) {
            const char *t = close + 2;
            const char *colon = strstr(t, ": ");
            if (colon != NULL && (size_t)(colon - t) < sizeof tag) {
                memcpy(tag, t, (size_t)(colon - t));
                tag[colon - t] = '\0';
                msg = colon + 2;
            } else {
                msg = t;
            }
        }
    }
    /* 160-byte message cap, cut back to a UTF-8 boundary (the JSON writer
     * would otherwise render the torn sequence as U+FFFD). */
    char cut[DIAG_LOG_MSG_MAX + 1];
    size_t mlen = strlen(msg);
    if (mlen > DIAG_LOG_MSG_MAX) {
        mlen = DIAG_LOG_MSG_MAX;
        while (mlen > 0 && ((unsigned char)msg[mlen] & 0xC0) == 0x80) mlen--;
    }
    memcpy(cut, msg, mlen);
    cut[mlen] = '\0';
    admit(now, tag, cut);
}

void diag_log_set_next(diag_log_vprintf_fn next)
{
    s_next = next;
}

static int forward(const char *fmt, va_list ap)
{
    /* Until install() has stored the previous sink (a few instructions after
     * esp_log_set_vprintf returns), fall back to plain stdout. */
    return s_next != NULL ? s_next(fmt, ap) : vprintf(fmt, ap);
}

int diag_log_vprintf(const char *fmt, va_list ap)
{
    /* Fast path: every non-error line goes straight down the chain with no
     * lock, no guard and no formatting. */
    if (!level_is_error(fmt)) return forward(fmt, ap);
    if (diag_port_in_isr()) {
        diag_port_lock();
        sat_inc(&s_dropped);
        diag_port_unlock();
        return forward(fmt, ap);
    }
    void *me = diag_port_task();
    /* Enter BEFORE forwarding, so a line logged from inside a downstream
     * sink while it handles ours is recognised as recursion. That nested
     * line still reaches the sinks (forward below); only its capture is
     * dropped and counted. */
    const guard_t g = guard_enter(me);
    va_list down;
    va_copy(down, ap);
    int ret = forward(fmt, down);
    va_end(down);
    if (g == GUARD_OK) {
        capture(fmt, ap);
        guard_exit(me);
    }
    return ret;
}

/* ── drain (task context) ──────────────────────────────────────────────── */

void diag_log_note_clock_valid(int64_t now_uptime_us)
{
    diag_port_lock();
    if (s_clock_valid_since_us < 0) s_clock_valid_since_us = now_uptime_us;
    diag_port_unlock();
}

bool diag_log_seal(diag_log_batch_t *b, int64_t now_uptime_us)
{
    if (b == NULL) return false;
    memset(b, 0, sizeof *b);
    bool sealed = false;
    diag_port_lock();
    const bool too_soon = s_have_record &&
                          now_uptime_us - s_last_record_us < DIAG_LOG_MIN_INTERVAL_US;
    const bool empty = s_count == 0 && s_dropped == 0 && s_rate_limited == 0;
    if (!too_soon && !empty && s_sealed == 0) {
        s_sealed = s_count;
        b->n = s_count;
        b->dropped = s_dropped;
        b->rate_limited = s_rate_limited;
        sealed = true;
    }
    diag_port_unlock();
    return sealed;
}

/* Room kept for the closing `],"dropped":N,"rate_limited":N}` at u32 max. */
#define DIAG_LOG_TAIL_RESERVE 64U

int diag_log_render(diag_log_batch_t *b, const diag_log_render_ctx_t *ctx, char *out, size_t cap)
{
    if (b == NULL || ctx == NULL || out == NULL || cap == 0) return -1;
    const size_t limit = cap < DIAG_LOG_RECORD_CAP + 1U ? cap : DIAG_LOG_RECORD_CAP + 1U;
    diag_port_lock();
    const int64_t valid_since = s_clock_valid_since_us;
    diag_port_unlock();
    diag_jw_t w;
    diag_jw_init(&w, out, limit);
    /* schema FIRST: the publisher routes canonical v3 families by this exact
     * prefix (payload_v3_is_canonical_object) without reparsing. */
    diag_jw_raw(&w, "{\"schema\":\"ambyte.log/1\",\"measure_id\":%lld,\"device\":",
                (long long)ctx->measure_id);
    diag_jw_str(&w, ctx->device != NULL ? ctx->device : "");
    diag_jw_raw(&w, ",\"tag\":\"LOG\",\"time\":{\"observed_utc\":%lld},\"boot_count\":%lu,\"lines\":[",
                (long long)ctx->observed_utc_ms, (unsigned long)ctx->boot_count);
    if (!w.ok) return -1;
    b->used = 0;
    for (uint32_t i = 0; i < b->n; i++) {
        /* Sealed entries are immutable and s_tail moves only in commit (this
         * task), so they are read without the lock. */
        const diag_line_t *e = &s_ring[(s_tail + i) % DIAG_LOG_RING_LINES];
        const size_t mark = w.len;
        diag_jw_raw(&w, "%s{\"uptime_ms\":%lld,\"utc_ms\":", i ? "," : "",
                    (long long)(e->uptime_us / 1000));
        if (ctx->clock_valid && valid_since >= 0 && e->uptime_us >= valid_since) {
            /* Back-computed from the drain-time clock along the monotonic
             * uptime axis: exact unless the wall clock was stepped in between,
             * in which case it reflects the corrected clock. */
            diag_jw_raw(&w, "%lld", (long long)(ctx->now_utc_ms - (ctx->now_uptime_us - e->uptime_us) / 1000));
        } else {
            diag_jw_raw(&w, "null");
        }
        diag_jw_raw(&w, ",\"level\":\"%c\",\"tag\":", e->level);
        diag_jw_str(&w, e->tag);
        diag_jw_raw(&w, ",\"msg\":");
        diag_jw_str(&w, e->msg);
        diag_jw_raw(&w, ",\"repeat\":%lu}", (unsigned long)e->repeat);
        if (!w.ok || w.len + DIAG_LOG_TAIL_RESERVE >= limit) {
            diag_jw_rewind(&w, mark);     /* this line waits for the next record */
            break;
        }
        b->used++;
    }
    diag_jw_raw(&w, "],\"dropped\":%lu,\"rate_limited\":%lu}",
                (unsigned long)b->dropped, (unsigned long)b->rate_limited);
    return w.ok ? (int)w.len : -1;
}

void diag_log_commit(const diag_log_batch_t *b, int64_t now_uptime_us)
{
    if (b == NULL) return;
    diag_port_lock();
    const uint32_t used = b->used <= s_count ? b->used : s_count;
    s_tail = (s_tail + used) % DIAG_LOG_RING_LINES;
    s_count -= used;
    s_sealed = 0;
    /* Counters may have grown since the seal: subtract only what went out. */
    s_dropped = s_dropped >= b->dropped ? s_dropped - b->dropped : 0;
    s_rate_limited = s_rate_limited >= b->rate_limited ? s_rate_limited - b->rate_limited : 0;
    s_have_record = true;
    s_last_record_us = now_uptime_us;
    sat_inc(&s_total_records);
    diag_port_unlock();
}

void diag_log_unseal(void)
{
    diag_port_lock();
    s_sealed = 0;
    diag_port_unlock();
}

void diag_log_stats(diag_log_stats_t *out)
{
    if (out == NULL) return;
    diag_port_lock();
    out->buffered = s_count;
    out->dropped = s_dropped;
    out->rate_limited = s_rate_limited;
    out->total_captured = s_total_captured;
    out->total_repeats = s_total_repeats;
    out->total_records = s_total_records;
    diag_port_unlock();
}

#ifdef DIAG_LOG_HOST
void diag_log_reset_for_test(void)
{
    diag_port_lock();
    memset(s_ring, 0, sizeof s_ring);
    memset(s_active, 0, sizeof s_active);
    s_tail = s_count = s_sealed = 0;
    s_dropped = s_rate_limited = 0;
    s_total_captured = s_total_repeats = s_total_records = 0;
    memset(&s_min_bucket, 0, sizeof s_min_bucket);
    memset(&s_hour_bucket, 0, sizeof s_hour_bucket);
    s_have_record = false;
    s_last_record_us = 0;
    s_clock_valid_since_us = -1;
    diag_port_unlock();
    s_next = NULL;
}

void diag_log_flush_for_test(void)
{
    diag_port_lock();
    s_tail = s_count = s_sealed = 0;
    s_dropped = s_rate_limited = 0;
    diag_port_unlock();
}
#endif
