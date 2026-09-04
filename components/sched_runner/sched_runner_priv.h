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

/* Lifecycle transition model. The firmware keeps one instance under its
 * portMUX; keeping the transition rules pure makes the stop-success → immediate
 * start contract host-testable without pretending to emulate FreeRTOS.
 *
 * A completion semaphore is only a wake hint. `completed_generation` is the
 * truth: an old task's late give can wake a waiter for a newer run, but it can
 * never complete that run. `sched_lifecycle_complete()` publishes STOPPED and
 * the completed generation in one lock-held transition before the task gives
 * the semaphore. */
typedef enum {
    SCHED_LIFE_STOPPED = 0,
    SCHED_LIFE_STARTING,
    SCHED_LIFE_RUNNING,
    SCHED_LIFE_STOPPING,
} sched_lifecycle_state_t;

typedef struct {
    sched_lifecycle_state_t state;
    uint32_t generation;
    uint32_t completed_generation;
    bool stop_requested;
} sched_lifecycle_t;

#define SCHED_LIFECYCLE_INITIALIZER \
    { SCHED_LIFE_STOPPED, 0, 0, false }

static inline bool sched_lifecycle_begin_start(sched_lifecycle_t *life,
                                                uint32_t *generation)
{
    if (life == NULL || life->state != SCHED_LIFE_STOPPED) return false;
    life->generation++;
    if (life->generation == 0) life->generation++; /* reserve zero as "none" */
    life->state = SCHED_LIFE_STARTING;
    life->stop_requested = false;
    if (generation != NULL) *generation = life->generation;
    return true;
}

static inline bool sched_lifecycle_publish_start(sched_lifecycle_t *life,
                                                  uint32_t generation,
                                                  bool *stop_requested)
{
    if (life == NULL || life->state != SCHED_LIFE_STARTING ||
        life->generation != generation) return false;
    if (stop_requested != NULL) *stop_requested = life->stop_requested;
    life->state = life->stop_requested ? SCHED_LIFE_STOPPING : SCHED_LIFE_RUNNING;
    return true;
}

/* Returns the generation whose completion a stopper must observe, or zero
 * when already stopped. */
static inline uint32_t sched_lifecycle_request_stop(sched_lifecycle_t *life)
{
    if (life == NULL || life->state == SCHED_LIFE_STOPPED) return 0;
    if (life->state == SCHED_LIFE_STARTING) life->stop_requested = true;
    if (life->state == SCHED_LIFE_RUNNING) life->state = SCHED_LIFE_STOPPING;
    return life->generation;
}

static inline bool sched_lifecycle_complete(sched_lifecycle_t *life,
                                             uint32_t generation)
{
    if (life == NULL || generation == 0 || life->generation != generation ||
        life->state == SCHED_LIFE_STOPPED) return false;
    life->completed_generation = generation;
    life->state = SCHED_LIFE_STOPPED;
    life->stop_requested = false;
    return true;
}

static inline bool sched_lifecycle_generation_complete(const sched_lifecycle_t *life,
                                                        uint32_t generation)
{
    if (life == NULL || generation == 0 || life->completed_generation == 0) {
        return false;
    }
    /* RFC-1982-style serial comparison: the latest completion proves every
     * generation at most half the uint32_t space behind it, including across
     * UINT32_MAX -> 1 (zero is reserved). A completion behind `generation`
     * has a large unsigned delta and therefore remains stale for that waiter.
     * The runner can have only one live generation, so a 2^31-run ambiguity
     * between a stop request and its completion is impossible in practice. */
    return life->completed_generation - generation < UINT32_C(0x80000000);
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
    bool     skipped_saturated;
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

/* Resolve the runner-owned $job.* subset. Returns false for a non-job
 * placeholder so the firmware callback can handle device facts. Pure and
 * host-tested, including the exact-vs-lower-bound saturation companion. */
bool sched_jw_job_placeholder(sched_jw_t *w, const char *ph,
                              const sched_runner_act_ctx_t *ctx);

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
