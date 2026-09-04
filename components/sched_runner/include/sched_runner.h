#ifndef AMBYTE_SCHED_RUNNER_H
#define AMBYTE_SCHED_RUNNER_H

/*
 * sched_runner — executes a compiled sched_program_t on a dedicated task.
 *
 * Lifecycle contract is identical to lua_runner's (the runner replaces it):
 * start/stop(wait_ms)/is_running, with stop() returning ESP_ERR_TIMEOUT when
 * the task is still inside a UART transaction — the task then exits on its
 * own once that call returns, and a later start() returns
 * ESP_ERR_INVALID_STATE until it has. app_main's five call sites (boot, OTA
 * workload suspend/resume, SD park, status-LED probe) drive this API exactly
 * as they drove lua_runner.
 *
 * The schedule source is /littlefs/schedule.yaml; when it is absent the
 * embedded default (components/sched_runner/default.yaml) runs instead, and
 * when it fails to compile the embedded default runs as EMBEDDED_FALLBACK
 * with the compile reason kept for `schedule status`. There is deliberately
 * no SD path anywhere: the embedded default covers a blank unit.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sched_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the running program came from (design decision 4: fallback is
 * firmware-owned embedded YAML, distinct from the released catalog). */
typedef enum {
    SCHED_SOURCE_NONE = 0,         /* never started */
    SCHED_SOURCE_INSTALLED,        /* /littlefs/schedule.yaml compiled clean */
    SCHED_SOURCE_EMBEDDED_DEFAULT, /* no installed file — embedded default */
    SCHED_SOURCE_EMBEDDED_FALLBACK,/* installed file invalid — embedded default */
} sched_source_kind_t;

typedef struct {
    sched_source_kind_t kind;
    char     sha256[65];  /* hex SHA-256 of the exact bytes that were compiled */
    char     reason[160]; /* compile error for EMBEDDED_FALLBACK, else "" */
} sched_source_t;

/* Runner-owned per-job counters. `skipped` is mirrored out of the due model
 * (slots missed past late grace); the rest are counted at execution. */
typedef struct {
    uint32_t runs;
    uint32_t failures;
    uint32_t skipped;
    uint32_t fail_streak;
    int64_t  last_run_epoch;   /* UTC seconds; -1 = never ran */
    uint32_t last_duration_ms;
} sched_job_stats_t;

/* One row of the `schedule status` job table. */
typedef struct {
    char     name[32];
    int64_t  next_due_utc;     /* UTC seconds; -1 = no scheduled firing */
    bool     boot_pending;     /* boot trigger not yet consumed */
    sched_job_stats_t stats;
} sched_job_status_t;

/* Load and start. Compiles /littlefs/schedule.yaml, falling back to the
 * embedded default as above; reserves the shared AMBIT trace buffer while the
 * heap is contiguous; applies NVS lat/lon to time_sync when provisioned;
 * spawns the runner task (priority 10, core 1 — the rationale comment moved
 * from lua_runner). ESP_ERR_INVALID_STATE when already running. */
esp_err_t sched_runner_start(void);

/* Signal stop and wait up to wait_ms. Actions poll
 * sched_runner_should_stop() between UART transactions and inside poll loops,
 * so a stop normally lands within one poll interval plus one transaction,
 * inside the 5 s budget app_main uses. ESP_ERR_TIMEOUT when a long UART call
 * is still in flight; the task exits on its own afterwards. No-op when not
 * running. */
esp_err_t sched_runner_stop(uint32_t wait_ms);

bool      sched_runner_is_running(void);

/* Queue a job for on-demand execution on the runner task (CLI `schedule run`,
 * MQTT schedule_run). ESP_ERR_NOT_FOUND for an unknown job, ESP_ERR_NO_MEM
 * when the 4-deep dispatch queue is full, ESP_ERR_INVALID_STATE when stopped.
 * Overlap policy applies when the job is already running or queued. */
esp_err_t sched_runner_dispatch(const char *job);

/* Actions poll this between UART transactions. */
bool      sched_runner_should_stop(void);

/* Source of the currently loaded program (valid after the first start). */
void      sched_runner_source(sched_source_t *out);

/* Stats for one job; ESP_ERR_NOT_FOUND when the name is unknown. */
esp_err_t sched_runner_stats(const char *job, sched_job_stats_t *out);

/* Program/header access for the CLI. sched_runner_program() is NULL before
 * the first successful start; the program is const once loaded. */
const sched_program_t *sched_runner_program(void);
int       sched_runner_job_count(void);
esp_err_t sched_runner_job_status(int idx, sched_job_status_t *out);

/* Compile-check a file without touching the running program (CLI
 * `schedule validate` on the staged /littlefs/schedule.yaml.new). err
 * receives the line-numbered reason on failure. */
esp_err_t sched_runner_validate_file(const char *path, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif
