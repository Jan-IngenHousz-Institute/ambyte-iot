/*
 * sched_runner_priv.h — state shared between sched_runner.c (task/lifecycle)
 * and sched_runner_actions.c (the action run functions). Component-private.
 */

#ifndef AMBYTE_SCHED_RUNNER_PRIV_H
#define AMBYTE_SCHED_RUNNER_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sched_spec.h"

/* Failure-streak log throttle (runner-owned, never a YAML knob — flood
 * protection is not optional; the numbers come from legacy_1Hz_spec.lua's fix
 * for the 536 K-line log flood): log the FIRST failure of a streak, every
 * 300th while it persists, and the recovery. Actions never log routine
 * failures themselves — they write fail_detail and run_job is the single
 * logging path. Pure + inline so the host test (tests/sched_runner_host.c)
 * can drive a 1,000-failure streak. */
#define SCHED_FAIL_STREAK_LOG_EVERY 300u

typedef enum {
    SCHED_FAIL_LOG_NONE = 0,
    SCHED_FAIL_LOG_FAILURE,   /* first failure, then every 300th */
    SCHED_FAIL_LOG_RECOVERY,  /* first success after a nonzero streak */
} sched_fail_log_t;

static inline sched_fail_log_t sched_fail_log_decision(bool failed,
                                                       uint32_t streak_after)
{
    if (!failed) {
        return streak_after > 0 ? SCHED_FAIL_LOG_RECOVERY : SCHED_FAIL_LOG_NONE;
    }
    return (streak_after == 1 ||
            streak_after % SCHED_FAIL_STREAK_LOG_EVERY == 0)
               ? SCHED_FAIL_LOG_FAILURE
               : SCHED_FAIL_LOG_NONE;
}

/* Snapshot of the job currently executing, refreshed before each run. The
 * actions read it for log lines and the db/store-event $job.* placeholders.
 * Single runner task → no locking. fail_detail is the action-written failure
 * context run_job logs when the throttle decides to (cleared per run). */
typedef struct {
    int      job_idx;      /* -1 when idle */
    const char *job_name;  /* pool string, valid for the program's lifetime */
    int64_t  boot_epoch;   /* UTC at runner start ($boot_epoch) */
    char     deployment[64]; /* device_config deployment tag, "" when unset */
    uint32_t runs;
    uint32_t failures;
    uint32_t skipped;
    uint32_t fail_streak;
    char     fail_detail[128];
} sched_runner_act_ctx_t;

extern sched_runner_act_ctx_t s_act_ctx;

/* ── db/store-event JSON writer (sched_runner_json.c) ──────────────────────
 * Pure: no firmware deps, so the host test (tests/sched_runner_host.c)
 * compiles it and drives the exact hardware-crash path from the T3 review.
 * $-placeholders resolve through the injected callback. */

typedef struct {
    char  *buf;
    size_t cap, len;
    bool   overflow;
} sched_jw_t;

typedef void (*sched_jw_placeholder_fn)(sched_jw_t *w, const char *ph,
                                        const sched_runner_act_ctx_t *ctx);

void sched_jw_raw(sched_jw_t *w, const char *fmt, ...);
/* Writes s as a JSON string with quote/backslash escaping; NULL → "" (a
 * compiled schedule must never be able to crash the runner). */
void sched_jw_str(sched_jw_t *w, const char *s);

/* Serialize one flat map input ("data"/"metadata") as a JSON object.
 * extra_key/extra_val prepend a member (store-event stamps data.kind);
 * extra_val == NULL OMITS the member. Returns false on overflow. */
bool sched_build_map_json(char *buf, size_t cap, const char *input_name,
                          const char *extra_key, const char *extra_val,
                          const sched_step_t *step, const sched_program_t *prog,
                          const sched_runner_act_ctx_t *ctx,
                          sched_jw_placeholder_fn placeholder);

/* Bind every catalog action's run function (sched_action_bind). Idempotent;
 * called by sched_runner_start(). */
void sched_runner_bind_actions(void);

#endif
