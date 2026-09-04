/*
 * sched_runner_priv.h — state shared between sched_runner.c (task/lifecycle)
 * and sched_runner_actions.c (the action run functions). Component-private.
 */

#ifndef AMBYTE_SCHED_RUNNER_PRIV_H
#define AMBYTE_SCHED_RUNNER_PRIV_H

#include <stdint.h>

/* Snapshot of the job currently executing, refreshed before each run. The
 * actions read it for log lines and the db/store-event $job.* placeholders.
 * Single runner task → no locking. */
typedef struct {
    int      job_idx;      /* -1 when idle */
    const char *job_name;  /* pool string, valid for the program's lifetime */
    int64_t  boot_epoch;   /* UTC at runner start ($boot_epoch) */
    char     deployment[64]; /* device_config deployment tag, "" when unset */
    uint32_t runs;
    uint32_t failures;
    uint32_t skipped;
    uint32_t fail_streak;
} sched_runner_act_ctx_t;

extern sched_runner_act_ctx_t s_act_ctx;

/* Bind every catalog action's run function (sched_action_bind). Idempotent;
 * called by sched_runner_start(). */
void sched_runner_bind_actions(void);

#endif
