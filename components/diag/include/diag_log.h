#pragma once
/*
 * diag_log.h — ESP_LOG error lines → durable ambyte.log/1 event-log records
 * (remote diagnostics, 2026-11).
 *
 * WHY: until now an error line existed in two places only — the USB console
 * nobody watches in the field, and /sdcard/logs/ambyte.log on a card nobody
 * copies. This captures level-E lines into a fixed RAM ring and a low-rate
 * drainer turns them into ordinary event-log records, so they ride the normal
 * at-least-once QoS1 path to clean_data like every measurement.
 *
 * ── Hook chain (explicit order) ───────────────────────────────────────────
 *   esp_log_write ──▶ diag_log_vprintf ──▶ sd_log_vprintf ──▶ console vprintf
 *                     (this; installed    (sd_logger.c;     (IDF default)
 *                      LAST in app_main)   installed first)
 * diag_log_install() is called right after sd_logger_init(), so the previous
 * sink it saves is sd_logger's hook, which in turn tees to the console: SD
 * logging and the console see every line exactly as before, and THEN the
 * error line is captured. If sd_logger failed to start, the previous sink is
 * the console itself. The chain is never re-ordered at runtime.
 *
 * ── The hook runs on ANY task ─────────────────────────────────────────────
 * Including the esp-mqtt task (which may touch only the portMUX latch table
 * and the completion queue — repo CLAUDE.md task boundaries) and tasks that
 * hold event_log's s_mtx. So the hook (diag_log_hook.c, its own object so the
 * claim is checkable — tools/check_diag_hook_symbols.py):
 *   - never locks s_mtx, never calls event_log, never allocates, never blocks:
 *     it formats into a stack buffer with diag_fmt (allocation-free; newlib's
 *     full vsnprintf would malloc in _dtoa_r for any %f/%e/%g) and copies
 *     under ONE portMUX (spinlock, a few hundred bytes of memcpy/strcmp, no
 *     calls out);
 *   - does not call time()/gettimeofday() (IDF takes a _lock there); lines are
 *     stamped with esp_timer uptime only and converted to UTC by the drainer;
 *   - drops and counts when the ring is full, in ISR context, or on
 *     recursion (a log emitted while this task is already inside the hook,
 *     e.g. from a downstream sink) — never waits.
 *
 * ── Rate limit (token buckets, provable window caps) ──────────────────────
 * A line costs one token from BOTH buckets:
 *   minute bucket: burst 20, +1 token / 6 s   → ≤ 20 + 10 = 30 lines in any 60 s
 *   hour bucket:   burst 60, +1 token / 15 s  → ≤ 60 + 240 = 300 lines in any hour
 * (A bucket with burst B and refill rate r admits at most B + r·W in any
 * window W, so the parameters are chosen to make the ticket's ≤30/min and
 * ≤300/h hold for EVERY window, not just on average.) Identical consecutive
 * lines (same level, tag and message) coalesce into the newest unsealed entry's
 * `repeat` and cost no token: an error loop costs one line.
 *
 * ── Drain (task context only) ─────────────────────────────────────────────
 * device_commands' cmd_store_log_event() — called from the sync_runner
 * watchdog task's once-a-minute tick — seals the ring, renders ONE record
 * (< 4 KiB) through the normal producer API (next_id + store_event: ids are
 * allocated under event_log's mutex, the cursor is never touched), then
 * commits (removes exactly the rendered lines) or unseals on failure. At most
 * one record per 60 s. Lines that do not fit stay for the next record.
 * Exactly-once: a sealed entry can no longer be coalesced into, so every
 * captured occurrence is counted in exactly one record's `repeat`, or in a
 * `dropped` / `rate_limited` count.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ≥ the minute bucket's burst (20) plus the few lines a full-size record can
 * leave behind; 24 × ~200 B ≈ 4.8 KiB of .bss (DRAM is the scarce resource
 * on PSRAM-less units, so not more). */
#define DIAG_LOG_RING_LINES    24U
#define DIAG_LOG_TAG_MAX       24U     /* incl. NUL */
#define DIAG_LOG_MSG_MAX       160U    /* bytes of formatted message (ticket: 160) */
/* Whole stored payload stays strictly below 4 KiB (ticket). Far below
 * EVLOG_RECORD_CAP_NORMAL / the 12 KB PSRAM-less record cap, so the cap
 * chain (AMBIT_RUN_PAYLOAD_CAP < EVLOG_RECORD_CAP_NORMAL < publish max) is
 * untouched. */
#define DIAG_LOG_RECORD_CAP    4095U
#define DIAG_LOG_MIN_INTERVAL_US  (60LL * 1000 * 1000)

#define DIAG_LOG_MIN_BURST     20U
#define DIAG_LOG_MIN_REFILL_US (6LL * 1000 * 1000)
#define DIAG_LOG_HOUR_BURST    60U
#define DIAG_LOG_HOUR_REFILL_US (15LL * 1000 * 1000)

typedef int (*diag_log_vprintf_fn)(const char *fmt, va_list ap);

/* The hook itself. Public so the host test drives the production function. */
int diag_log_vprintf(const char *fmt, va_list ap);

/* Set the sink the hook forwards every line to (the previous vprintf).
 * diag_log_install() does esp_log_set_vprintf() + this on the target. */
void diag_log_set_next(diag_log_vprintf_fn next);
/* Target only (diag_boot.c): chain onto the current esp_log sink. Idempotent. */
void diag_log_install(void);

/* The drainer's view of the clock (task context only). Lines captured at or
 * after the first uptime at which the system clock was known valid get
 * utc_ms; earlier lines carry utc_ms:null (ticket: pre-time-sync lines). */
void diag_log_note_clock_valid(int64_t now_uptime_us);

typedef struct {
    uint32_t n;              /* entries sealed for this record */
    uint32_t used;           /* entries actually rendered (≤ n) */
    uint32_t dropped;        /* counter snapshots at seal time */
    uint32_t rate_limited;
    uint32_t unsupported_fmt;
} diag_log_batch_t;

typedef struct {
    int64_t     measure_id;
    const char *device;              /* MAC string */
    int64_t     observed_utc_ms;     /* record time (telemetry convention) */
    uint32_t    boot_count;          /* 0 = unknown */
    int64_t     now_uptime_us;
    bool        clock_valid;
    int64_t     now_utc_ms;
} diag_log_render_ctx_t;

/* Seal everything currently in the ring. False (and nothing sealed) when
 * there is nothing to report or the last record is < 60 s old. */
bool diag_log_seal(diag_log_batch_t *b, int64_t now_uptime_us);
/* Render one ambyte.log/1 object (< DIAG_LOG_RECORD_CAP) from the sealed
 * entries, as many lines as fit; sets b->used. Returns length or -1. */
int  diag_log_render(diag_log_batch_t *b, const diag_log_render_ctx_t *ctx, char *out, size_t cap);
/* The record is durable: drop the rendered lines + the snapshot counts. */
void diag_log_commit(const diag_log_batch_t *b, int64_t now_uptime_us);
/* The record was not stored: keep everything for the next attempt. */
void diag_log_unseal(void);

typedef struct {
    uint32_t buffered;        /* lines waiting in the ring */
    uint32_t dropped;         /* not yet reported in a record */
    uint32_t rate_limited;
    uint32_t unsupported_fmt; /* lines whose format used a %a/%n/wide/unknown conversion */
    uint32_t total_captured;  /* since boot: lines admitted (excl. repeats) */
    uint32_t total_repeats;   /* since boot: occurrences coalesced */
    uint32_t total_records;   /* since boot: records committed */
} diag_log_stats_t;
void diag_log_stats(diag_log_stats_t *out);

#ifdef DIAG_LOG_HOST
/* tests/diag_host only (not compiled into firmware): reset everything /
 * empty the ring while keeping the rate-limit buckets. */
void diag_log_reset_for_test(void);
void diag_log_flush_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
