/* HIL-only relevant-operation trace + fault arming. See evq_hil_trace.h.
 * Pure C; the host test IO-1 compiles this exact file. Callers serialize:
 * every recording site runs inside event_log (under its mutex or on the
 * keeper, one at a time) — a tiny critical-section hook keeps the ring
 * coherent even when two event_log paths overlap. */
#include "evq_hil_trace.h"

#include <stdio.h>
#include <string.h>

#ifdef EVQ_HIL_TRACE_HOST
#define TR_LOCK()   ((void)0)
#define TR_UNLOCK() ((void)0)
#else
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_tr_mux = portMUX_INITIALIZER_UNLOCKED;
#define TR_LOCK()   portENTER_CRITICAL(&s_tr_mux)
#define TR_UNLOCK() portEXIT_CRITICAL(&s_tr_mux)
#endif

static evq_tr_entry_t *s_ring;
static uint64_t s_next_seq = 1;          /* seq of the next recorded entry */
static uint64_t s_drained_upto = 0;      /* last seq handed to a drain */
static evq_io_counters_t s_ctr;

static bool starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static const char *strip_dot(const char *p)
{
    return (p[0] == '.' && p[1] == '/') ? p + 2 : p;
}

bool evq_tr_is_sd(const char *path)
{
    if (path == NULL) return false;
    const char *p = strip_dot(path);
    return starts_with(p, "/sdcard") || starts_with(p, "sdcard");
}

bool evq_tr_is_sd_record(const char *path)
{
    if (!evq_tr_is_sd(path)) return false;
    const char *p = strchr(strip_dot(path) + 1, '/');
    if (p == NULL) return false;
    return starts_with(p, "/events") || starts_with(p, "/evq") || starts_with(p, "/archive");
}

bool evq_tr_is_index(const char *path)
{
    return path != NULL && strstr(path, "evq.idx") != NULL;
}

bool evq_tr_is_flash_segment(const char *path)
{
    if (path == NULL || evq_tr_is_sd(path)) return false;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    return strstr(path, "/events/") != NULL && starts_with(b, "ev-");
}

static void tail_copy(char *dst, const char *src)
{
    dst[0] = '\0';
    if (src == NULL) return;
    size_t n = strlen(src);
    const char *s = n >= EVQ_TR_NAME_MAX ? src + n - (EVQ_TR_NAME_MAX - 1) : src;
    snprintf(dst, EVQ_TR_NAME_MAX, "%s", s);
}

void evq_tr_init(evq_tr_entry_t *storage)
{
    TR_LOCK();
    s_ring = storage;
    s_next_seq = 1;
    s_drained_upto = 0;
    memset(&s_ctr, 0, sizeof s_ctr);
    TR_UNLOCK();
}

void evq_tr_record(int64_t us, evq_tr_op_t op, int result, uint32_t bytes, const char *a, const char *b)
{
    if (s_ring == NULL) return;
    evq_tr_entry_t e;
    e.us = us;
    e.op = (uint8_t)op;
    e.result = (int8_t)(result < -127 ? -127 : (result > 127 ? 127 : result));
    e.bytes = bytes;
    tail_copy(e.a, a);
    tail_copy(e.b, b);
    TR_LOCK();
    e.seq = s_next_seq++;
    s_ring[(e.seq - 1) % EVQ_TR_CAP] = e;
    TR_UNLOCK();
}

uint64_t evq_tr_total(void)
{
    TR_LOCK();
    uint64_t t = s_next_seq - 1;
    TR_UNLOCK();
    return t;
}

size_t evq_tr_drain(void (*emit)(const evq_tr_entry_t *e, void *ctx), void *ctx,
                    uint64_t *out_lost, uint64_t *out_first, uint64_t *out_last)
{
    uint64_t lost = 0, first = 0, last = 0;
    size_t n = 0;
    if (s_ring == NULL) {
        if (out_lost) *out_lost = 0;
        if (out_first) *out_first = 0;
        if (out_last) *out_last = 0;
        return 0;
    }
    TR_LOCK();
    uint64_t end = s_next_seq - 1;            /* newest recorded */
    uint64_t start = s_drained_upto + 1;      /* first not yet drained */
    uint64_t oldest = end >= EVQ_TR_CAP ? end - EVQ_TR_CAP + 1 : 1;
    if (start < oldest) { lost = oldest - start; start = oldest; }
    s_drained_upto = end;
    TR_UNLOCK();
    /* Entries in [start, end] may be overwritten while we emit if producers
     * run concurrently; copy each under the lock and re-check its seq. */
    for (uint64_t q = start; q <= end && end >= start; q++) {
        evq_tr_entry_t e;
        TR_LOCK();
        e = s_ring[(q - 1) % EVQ_TR_CAP];
        TR_UNLOCK();
        if (e.seq != q) { lost++; continue; }
        if (first == 0) first = q;
        last = q;
        if (emit) emit(&e, ctx);
        n++;
    }
    if (out_lost) *out_lost = lost;
    if (out_first) *out_first = first;
    if (out_last) *out_last = last;
    return n;
}

evq_io_counters_t *evq_io_counters(void) { return &s_ctr; }

/* ── fault arming ──────────────────────────────────────────────────────── */

static char s_arm_point[64];
static evq_arm_mode_t s_arm_mode;
static unsigned s_arm_nth, s_arm_hits;
static int s_arm_io;                 /* pending io action: errno, or -1 = reset inside */

void evq_arm_set(const char *point, evq_arm_mode_t mode, unsigned nth)
{
    TR_LOCK();
    snprintf(s_arm_point, sizeof s_arm_point, "%s", point ? point : "");
    s_arm_mode = mode;
    s_arm_nth = nth ? nth : 1;
    s_arm_hits = 0;
    s_arm_io = 0;
    TR_UNLOCK();
}

void evq_arm_clear(void) { evq_arm_set("", EVQ_ARM_NONE, 1); }

evq_arm_mode_t evq_arm_hit(const char *point)
{
    evq_arm_mode_t act = EVQ_ARM_NONE;
    TR_LOCK();
    if (s_arm_mode != EVQ_ARM_NONE && point != NULL && strcmp(point, s_arm_point) == 0 &&
        ++s_arm_hits == s_arm_nth) {
        switch (s_arm_mode) {
        case EVQ_ARM_RESET:        act = EVQ_ARM_RESET; break;
        case EVQ_ARM_RESET_INSIDE: s_arm_io = -1; break;
        case EVQ_ARM_EIO:          s_arm_io = 5;  break;   /* EIO */
        case EVQ_ARM_ENOSPC:       s_arm_io = 28; break;   /* ENOSPC */
        default: break;
        }
        s_arm_mode = EVQ_ARM_NONE;   /* one shot */
    }
    TR_UNLOCK();
    return act;
}

int evq_arm_take_io(void)
{
    TR_LOCK();
    int v = s_arm_io;
    s_arm_io = 0;
    TR_UNLOCK();
    return v;
}

bool evq_arm_describe(char *buf, size_t cap)
{
    TR_LOCK();
    bool armed = s_arm_mode != EVQ_ARM_NONE || s_arm_io != 0;
    snprintf(buf, cap, "point=%s mode=%d nth=%u hits=%u pending_io=%d",
             s_arm_point[0] ? s_arm_point : "-", (int)s_arm_mode, s_arm_nth, s_arm_hits, s_arm_io);
    TR_UNLOCK();
    return armed;
}
