#pragma once
/*
 * diag_log_priv.h — state shared by the two halves of the error-log capture:
 *
 *   diag_log_hook.c  runs on ANY logging task (esp-mqtt, s_mtx holders, …):
 *                    level check, recursion guard, allocation-free formatting
 *                    (diag_fmt.c), parse, admit into the ring.
 *   diag_log.c       runs on the drainer (sync_runner watchdog task) only:
 *                    seal, render (diag_json.c, which uses newlib vsnprintf —
 *                    fine in task context), commit / unseal.
 *
 * The split is the evidence for the hook contract: tools/check_diag_hook_symbols.py
 * reads the undefined symbols of diag_log_hook.o and diag_fmt.o (target objects
 * in CI, host objects in tests/test_diag.py) and refuses any allocator, any
 * printf-family call, any newlib reentrant (_r) routine and any blocking lock.
 */
#include <stdbool.h>
#include <stdint.h>

#include "diag_log.h"

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

/* Recursion guard WITHOUT thread-local storage: the tasks currently inside
 * the hook. A task found here again is re-entering (a log emitted by a sink
 * downstream of us while we handle its line). More concurrent loggers than
 * slots also drop+count rather than wait. */
#define DIAG_LOG_ACTIVE_MAX 8U

/* All fields guarded by the port lock unless noted. */
typedef struct {
    diag_line_t   ring[DIAG_LOG_RING_LINES];
    uint32_t      tail;                  /* oldest entry; written only by the drainer */
    uint32_t      count;
    /* The oldest `sealed` entries belong to the record being built: producers
     * may neither coalesce into them nor reuse their slots. This is what makes
     * every occurrence land in exactly one record. */
    uint32_t      sealed;
    uint32_t      dropped;               /* not yet reported */
    uint32_t      rate_limited;          /* not yet reported */
    uint32_t      unsupported_fmt;       /* not yet reported (diag_fmt.h) */
    uint32_t      total_captured, total_repeats, total_records;
    diag_bucket_t min_bucket, hour_bucket;
    bool          have_record;
    int64_t       last_record_us;
    /* First uptime at which the wall clock was known valid; -1 = not yet. Set
     * from task context only (app_main, the SNTP callback, the drainer) but by
     * several tasks, and a 64-bit store is two words on Xtensa: lock-guarded. */
    int64_t       clock_valid_since_us;
    void         *active[DIAG_LOG_ACTIVE_MAX];
    diag_log_vprintf_fn next;            /* downstream sink (sd_logger → console) */
} diag_log_state_t;

extern diag_log_state_t g_dl;            /* defined in diag_log.c */

static inline void sat_inc(uint32_t *v) { if (*v != UINT32_MAX) (*v)++; }

/* Downstream sink used only in the few instructions between
 * esp_log_set_vprintf() returning and diag_log_set_next() storing the
 * previous sink. Defined outside the hook TU (diag_boot.c: plain vprintf; the
 * host tests: a counting stub) so the hook object references no printf. */
int diag_log_fallback_sink(const char *fmt, va_list ap);
