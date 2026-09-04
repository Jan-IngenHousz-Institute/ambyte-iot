#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "messaging_port.h"
#include "script_identity_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Remote schedule control over MQTT (docs/schedule-releases.md):
 *
 *  - script_update: replace /littlefs/schedule.yaml with an inline-delivered
 *    schedule and restart the schedule runner. The schedule is compiled BEFORE
 *    the filesystem is touched; the previous file survives as schedule.yaml.bak.
 *    Optional sha256 integrity check. NVS id latch (success only) dedupes the
 *    retained topic.
 * Work runs on the shared maintenance worker so it never blocks the MQTT task.
 */

/* Suspend/resume the schedule workload (app_workload_suspend/resume). The
 * `url` variant needs these: stopping the runner frees its buffers and defragments
 * the heap so the download's TLS handshake gets its contiguous record buffer. */
typedef void (*script_workload_fn)(void);

typedef struct {
    message_publish_fn      publish;        /* status/result reports; NULL = log-only */
    message_is_connected_fn is_connected;
    const char             *status_topic;
    const char             *device_id;
    script_workload_fn      workload_suspend;  /* NULL disables the url variant */
    script_workload_fn      workload_resume;
    script_workload_fn      comms_suspend;      /* mqtt_client_stop — free MQTT's TLS heap for the download */
    script_workload_fn      comms_resume;       /* mqtt_client_start — reconnect before reporting */
    /* Global maintenance lock: begin() returns false if another maintenance op
     * (any update type) is already running — the script op is then rejected as
     * "busy" rather than overlapping. end() releases it. Both NULL = no gate. */
    bool                  (*maintenance_begin)(void);
    void                  (*maintenance_end)(void);
    /* Submit the op to the shared maintenance worker (one resident task for ALL
     * update types, created while the heap is clean at boot). run(arg) runs the
     * op and must free(arg). Returns false if the worker queue is full. Replaces
     * the old on-demand task spawn that failed (ESP_ERR_NO_MEM) on the fragmented
     * field heap. Required (NULL = requests fail INVALID_STATE). */
    bool                  (*submit)(void (*run)(void *arg), void *arg);
} script_update_config_t;

/* Prepare the worker plumbing (idempotent; the task itself spawns on demand). */
esp_err_t script_update_init(const script_update_config_t *cfg);

/* Queue a schedule.yaml replacement. `script` is copied (caller's buffer may die);
 * `checksum` (optional, NULL ok) is the lowercase/uppercase hex SHA-256 of the
 * script; `id` dedupes a retained message (latched on success only; NULL = no
 * dedupe). `reboot` = true (the default) restarts the whole device after a
 * successful swap so the new script runs from a fresh boot; false keeps the old
 * in-place behaviour (stop + swap + restart just the schedule runner). Either way the
 * id is latched on success FIRST, so a retained trigger can't loop the reboot.
 * `script_version` and `built_against_fw` are optional release provenance;
 * legacy senders may pass NULL. Reports script_status with the active script
 * hash, verified provenance, install firmware, and current compiled firmware.
 * ESP_ERR_INVALID_STATE before init; ESP_ERR_NO_MEM if busy or out of memory. */
esp_err_t script_update_request(const char *script, const char *checksum, const char *id,
                                bool reboot, const char *script_version,
                                const char *built_against_fw);

/* Queue a schedule.yaml replacement fetched from `url` (HTTPS, streamed in 4 KB
 * chunks). Unlike the inline variant this needs NO large contiguous MQTT/TLS
 * buffer, so it lands reliably on a fragmented heap — the worker stops the runner first
 * to defragment, downloads, verifies (`checksum` = sha256 hex of the file, optional)
 * and compiles, then swaps + reboots/restarts. Requires workload_suspend/
 * resume in the config (else ESP_ERR_INVALID_STATE-style "unavailable" report).
 * `id`/`reboot`/release-provenance semantics match script_update_request. */
esp_err_t script_update_url_request(const char *url, const char *checksum, const char *id,
                                    bool reboot, const char *script_version,
                                    const char *built_against_fw);

/* Queue the same verified HTTPS replacement without the deterministic fleet
 * delay. This is for an operator attached to the local serial console; remote
 * MQTT callers should keep using script_update_url_request() so a fleet does
 * not begin TLS downloads simultaneously. */
esp_err_t script_update_url_request_immediate(const char *url, const char *checksum,
                                              const char *id, bool reboot,
                                              const char *script_version,
                                              const char *built_against_fw);

/* Where the console's `schedule put` stages bytes for script_update_local_request().
 * Same file the URL variant downloads into, so both share one install path.
 * On internal littlefs (alongside the active schedule), so a serial push works with no SD
 * card inserted. */
#define SCRIPT_UPDATE_STAGING_PATH "/littlefs/schedule.yaml.new"

/* Queue a schedule.yaml replacement from bytes ALREADY staged at
 * SCRIPT_UPDATE_STAGING_PATH by the console. Needs no network at all: the
 * operator's PC pushed the script over serial, so onboarding works on a bench
 * with no uplink. Verification is unchanged (sha256 of the staged file against
 * `checksum`, schedule compile, previous file kept as schedule.yaml.bak). Unlike the
 * URL variant this does not stop MQTT, since nothing here needs the TLS heap.
 * `id`/`reboot`/release-provenance semantics match script_update_request. */
esp_err_t script_update_local_request(const char *checksum, const char *id,
                                      bool reboot, const char *script_version,
                                      const char *built_against_fw);

/* Hash the active /littlefs/schedule.yaml and return release provenance only when the
 * stored release digest matches the file. Suitable as a script_identity_read_fn. */
esp_err_t script_update_get_identity(script_identity_t *out);

#ifdef __cplusplus
}
#endif
