/*
 * diag_log_hook.c — the esp_log vprintf hook: everything that runs on the
 * LOGGING task (any task, including the esp-mqtt task and holders of
 * event_log's s_mtx). Contract in include/diag_log.h; the drain half is
 * diag_log.c.
 *
 * Allowed here: the port spinlock, esp_timer uptime, the current task handle,
 * string/memory primitives, diag_fmt (allocation-free formatting) and the
 * downstream sink pointer. Nothing else — no heap, no newlib printf family,
 * no newlib _r routines, no FreeRTOS queue/semaphore, no event_log, no
 * time()/gettimeofday() (IDF takes a _lock there). tools/check_diag_hook_symbols.py
 * enforces that on the undefined symbols of this object and of diag_fmt.o.
 */
#include <string.h>

#include "diag_fmt.h"
#include "diag_log.h"
#include "diag_log_port.h"
#include "diag_log_priv.h"

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
        if (g_dl.active[i] == me) { g = GUARD_RECURSION; break; }
        if (g_dl.active[i] == NULL && free_slot < 0) free_slot = (int)i;
    }
    if (g != GUARD_RECURSION && free_slot >= 0) {
        g_dl.active[free_slot] = me;
        g = GUARD_OK;
    }
    if (g != GUARD_OK) sat_inc(&g_dl.dropped);
    diag_port_unlock();
    return g;
}

static void guard_exit(void *me)
{
    diag_port_lock();
    for (unsigned i = 0; i < DIAG_LOG_ACTIVE_MAX; i++) {
        if (g_dl.active[i] == me) { g_dl.active[i] = NULL; break; }
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
    if (g_dl.count > g_dl.sealed) {
        diag_line_t *nw = &g_dl.ring[(g_dl.tail + g_dl.count - 1U) % DIAG_LOG_RING_LINES];
        if (nw->level == 'E' && strcmp(nw->tag, tag) == 0 && strcmp(nw->msg, msg) == 0) {
            sat_inc(&nw->repeat);
            sat_inc(&g_dl.total_repeats);
            diag_port_unlock();
            return;
        }
    }
    if (g_dl.count >= DIAG_LOG_RING_LINES) {
        /* Full before spending a token: a line we cannot keep must not eat
         * the budget of the lines that come after the drain. */
        sat_inc(&g_dl.dropped);
        diag_port_unlock();
        return;
    }
    bucket_refill(&g_dl.min_bucket, DIAG_LOG_MIN_BURST, DIAG_LOG_MIN_REFILL_US, now);
    bucket_refill(&g_dl.hour_bucket, DIAG_LOG_HOUR_BURST, DIAG_LOG_HOUR_REFILL_US, now);
    if (g_dl.min_bucket.tokens == 0 || g_dl.hour_bucket.tokens == 0) {
        sat_inc(&g_dl.rate_limited);
        diag_port_unlock();
        return;
    }
    g_dl.min_bucket.tokens--;
    g_dl.hour_bucket.tokens--;
    diag_line_t *e = &g_dl.ring[(g_dl.tail + g_dl.count) % DIAG_LOG_RING_LINES];
    e->uptime_us = now;
    e->repeat = 1;
    e->level = 'E';
    strncpy(e->tag, tag, sizeof e->tag - 1);
    e->tag[sizeof e->tag - 1] = '\0';
    strncpy(e->msg, msg, sizeof e->msg - 1);
    e->msg[sizeof e->msg - 1] = '\0';
    g_dl.count++;
    sat_inc(&g_dl.total_captured);
    diag_port_unlock();
}

/* Format + parse + admit. Kept out of line so its ~260 B buffer lives only
 * AFTER the downstream sinks (sd_logger + console) have returned: the hook
 * adds its formatting frame next to theirs, never on top of them. */
static __attribute__((noinline)) void capture(const char *fmt, va_list ap)
{
    const int64_t now = diag_port_uptime_us();
    char buf[DIAG_LOG_TAG_MAX + DIAG_LOG_MSG_MAX + 72];
    /* NOT newlib vsnprintf: with full (non-nano) formatting it reaches
     * _dtoa_r → _Balloc → malloc for any %f/%e/%g, and this runs on the
     * esp-mqtt task and on s_mtx holders. diag_fmt never allocates. */
    uint32_t unsupported = 0;
    size_t len = diag_vsnformat(buf, sizeof buf, fmt, ap, &unsupported);
    if (unsupported != 0) {
        diag_port_lock();
        sat_inc(&g_dl.unsupported_fmt);
        diag_port_unlock();
    }
    len = strip_ansi(buf, len);
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
    g_dl.next = next;
}

static int forward(const char *fmt, va_list ap)
{
    /* Downstream sinks keep their pre-existing behaviour (newlib formatting
     * on this task, exactly as before this feature). Until install() has
     * stored the previous sink, a few instructions after esp_log_set_vprintf
     * returns, the fallback is plain stdout (defined outside this TU). */
    return g_dl.next != NULL ? g_dl.next(fmt, ap) : diag_log_fallback_sink(fmt, ap);
}

int diag_log_vprintf(const char *fmt, va_list ap)
{
    /* Fast path: every non-error line goes straight down the chain with no
     * lock, no guard and no formatting. */
    if (!level_is_error(fmt)) return forward(fmt, ap);
    if (diag_port_in_isr()) {
        diag_port_lock();
        sat_inc(&g_dl.dropped);
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
