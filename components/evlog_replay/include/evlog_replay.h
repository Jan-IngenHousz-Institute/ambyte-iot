#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "messaging_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Operator-driven re-send of records from the SD archive.
 *
 * Background: /sdcard/archive/arc-*.log holds verbatim copies of event_log
 * files whose records the broker ACKNOWLEDGED. On 16–18 Sep 2026 the broker
 * acknowledged publishes it had refused (missing topic policy), so those records
 * left the queue without ever reaching the platform. This module re-appends a
 * selected range of archived records to the live event store as PENDING, with
 * their original measure_id and capture times, so the normal FIFO publishes them
 * again. It never touches the read cursor or the in-flight window: replayed
 * records simply queue behind live ones.
 *
 * Selection: a measure_id range and/or a capture-time (start_ms) window. The
 * platform knows exactly which ids it is missing, so an id range is the precise
 * selector; the time window is a guard.
 *
 * Crash safety: appends happen in chunks; after each chunk the tail is fsync'd
 * and the next id is persisted in NVS. On a reset the job resumes; to avoid the
 * duplicate-chunk window it first asks event_log for the highest still-pending
 * id inside the range and resumes after it (exact unless that scan hit its
 * bound — the reply reports `resume_exact`). Re-running a finished token is a
 * no-op. Store protection: the job pauses while more than
 * EVLOG_REPLAY_PENDING_CAP records are pending or the store refuses the append
 * (full of unsynced data), so live captures are never refused because of a
 * replay. The free-space floor below is deliberately under event_log's own
 * eviction watermark (256 KiB, evicting synced files up to 512 KiB): in steady
 * state free space hovers inside that band, so a higher floor would never be
 * reached again once the store has filled and the job would pause forever.
 *
 * Provenance: the stored record carries no workbook provenance; the publisher
 * normally stamps the currently installed schedule. evlog_replay_covers() lets it
 * omit that for replayed ranges (the current job plus the last few completed).
 */

#define EVLOG_REPLAY_TOKEN_MAX       32
#define EVLOG_REPLAY_DEFAULT_CHUNK   64
#define EVLOG_REPLAY_MAX_CHUNK       1000
#define EVLOG_REPLAY_MIN_FREE_BYTES  (128u * 1024u)   /* must stay below event_log's EVLOG_MIN_FREE_BYTES */
#define EVLOG_REPLAY_PENDING_CAP     3000
#define EVLOG_REPLAY_HISTORY         4        /* completed ranges remembered for covers() */

typedef struct {
    char     token[EVLOG_REPLAY_TOKEN_MAX + 1];   /* [A-Za-z0-9._:-], operator-chosen job id */
    int64_t  from_id;   /* 0 = unbounded */
    int64_t  to_id;     /* 0 = unbounded */
    int64_t  from_ms;   /* 0 = unbounded, on the record's capture start_ms */
    int64_t  to_ms;     /* 0 = unbounded */
    uint32_t chunk;     /* appends per progress commit; 0 = default */
} evlog_replay_req_t;

typedef enum {
    EVLOG_REPLAY_IDLE = 0,
    EVLOG_REPLAY_RUNNING,
    EVLOG_REPLAY_DONE,
    EVLOG_REPLAY_CANCELLED,
    EVLOG_REPLAY_FAILED,
} evlog_replay_state_t;

typedef struct {
    message_publish_fn publish;          /* replies on status_topic */
    const char        *status_topic;
    const char        *device_id;
    const char        *firmware_version;
    void             (*notify_publisher)(void);   /* sync_runner_notify; NULL = none */
} evlog_replay_config_t;

/* Load the persisted job from NVS; if it was RUNNING, schedule its resume. Call
 * once after event_log_init succeeds and MQTT is initialised. */
esp_err_t evlog_replay_init(const evlog_replay_config_t *cfg);

/* Dry run: count archive records matching `req` (no writes), reply once. */
esp_err_t evlog_replay_count(const evlog_replay_req_t *req, const char *cmd_id);
/* Start (or acknowledge an existing) job for req->token. Reply once now, per
 * 1000 appended records, and on completion. */
esp_err_t evlog_replay_run(const evlog_replay_req_t *req, const char *cmd_id);
/* Stop the job with this token after the current record; reply once. Records
 * already re-appended stay queued (they are the missing data). */
esp_err_t evlog_replay_cancel(const char *token, const char *cmd_id);
/* Reply with the persisted job state. */
esp_err_t evlog_replay_status(const char *cmd_id);

/* True when `measure_id` lies inside the current job's range or one of the last
 * EVLOG_REPLAY_HISTORY completed ranges. Safe from any task; no allocation. */
bool evlog_replay_covers(int64_t measure_id);

#ifdef __cplusplus
}
#endif
