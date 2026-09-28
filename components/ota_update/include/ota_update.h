#pragma once

#include "esp_err.h"
#include "messaging_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MQTT-triggered self-OTA for the ambyte (docs/ota-update-plan.md, Stage 3).
 *
 * Two triggers: the custom command topic (command_router dispatches an
 * `ota_update` command to ota_update_request) and openJII's AWS IoT Jobs
 * rollout (iot_jobs calls ota_update_request_job, which adds version + sha256
 * checks). The download is Stage-0-proven `esp_https_ota` from an HTTPS URL
 * (a GitHub release asset, or a presigned S3 URL for a job).
 *
 * Heap: the device cannot hold two TLS sessions at once (~17 KB largest block).
 * So the worker SUSPENDS MQTT (frees its TLS) for the download, recreating the
 * quiesced max-heap state Stage 0 measured, then reboots. Status is reported
 * before suspend (`accepted`), after a failure (`failed`), or after the new
 * image reboots and reconnects (`success`).
 *
 * Rollback: requires the dual-OTA partition table + CONFIG_BOOTLOADER_APP_-
 * ROLLBACK_ENABLE (OTA Stage 1). The new image boots PENDING_VERIFY; this module
 * marks it valid only after MQTT reconnects (proving the image is healthy),
 * else forces a rollback. So a connectivity-breaking image reverts instead of
 * stranding the device.
 */

typedef struct {
    message_publish_fn      publish;        /* mqtt_client_get_publish_fn() — status reports */
    message_is_connected_fn is_connected;   /* mqtt_client_get_is_connected_fn() — health gate */
    void                  (*comms_suspend)(void);    /* mqtt_client_stop — free TLS heap for the DL */
    void                  (*comms_resume)(void);     /* mqtt_client_start — after a failed DL */
    void                  (*workload_suspend)(void); /* stop the schedule runner during the DL
                                                      * (frees heap, avoids fragmentation); NULL = skip */
    void                  (*workload_resume)(void);  /* restart it after a failed DL; NULL = skip */
    /* Global maintenance lock: begin() returns false if another maintenance op
     * (any update type) is already running — the OTA is then rejected as "dropped"
     * rather than overlapping (two TLS sessions → OOM on this no-PSRAM board).
     * end() releases it. Both NULL = no gate (always proceed). */
    bool                  (*maintenance_begin)(void);
    void                  (*maintenance_end)(void);
    /* Submit the OTA op to the shared maintenance worker (one resident task for
     * ALL update types, created while the heap is clean at boot). run(arg) runs
     * the op and must free(arg). Returns false if the worker queue is full. This
     * replaces a per-op task spawn that could fail (ESP_ERR_NO_MEM) on the
     * fragmented field heap. Required (NULL = requests fail INVALID_STATE). */
    bool                  (*submit)(void (*run)(void *arg), void *arg);
    /* Post-reboot health gate EXTENSION: returns true when SD/persistence is healthy
     * (or when the unit genuinely has no card). A just-applied image is marked valid
     * only if MQTT reconnects AND this returns true, so an image that reconnects but
     * breaks SD mounting / the event log rolls back instead of stranding the unit
     * measurement-dead (audit: the rollback gate was MQTT-only). NULL = skip the
     * check (preserves MQTT-only confirm). */
    bool                  (*persistence_healthy)(void);
    const char             *status_topic;   /* where status JSON is published */
    const char             *device_id;      /* included in status payloads */
    /* Called on the maintenance worker right after a just-applied image is
     * marked valid, so a job-driven update can report SUCCEEDED only once the
     * rollback window has closed (iot_jobs_kick). NULL = skip. */
    void                  (*confirmed)(void);
} ota_update_config_t;

/* A job-driven update (AWS IoT Jobs, iot_jobs.c). Unlike the command path it
 * carries the release's identity, which the worker enforces before the boot
 * partition switches:
 *   - expected_version must equal the downloaded image's own app version
 *     (leading 'v' ignored on both). A mismatched asset would otherwise install,
 *     look like "not that release" after reboot, and be refetched forever.
 *   - sha256 (64 hex) must equal the digest of the bytes actually on flash.
 * `started` fires once the maintenance lock is held and MQTT is still up (the
 * moment to report IN_PROGRESS). `finished` fires only when the update did NOT
 * reboot into a new image: retryable=true means it never ran (worker busy /
 * no memory) and the job is untouched; false is a real failure with `detail`.
 * The reboot-into-new-image outcome is resolved after boot via `confirmed`. */
typedef struct {
    const char *url;               /* HTTPS; may be a long presigned S3 URL */
    const char *id;                /* job id; latched as the applied id on success */
    const char *sha256;            /* required, 64 hex chars */
    const char *expected_version;  /* required */
    void (*started)(const char *id, void *ctx);
    void (*finished)(const char *id, bool retryable, const char *detail, void *ctx);
    void *ctx;
} ota_update_job_t;

/* Prepare the OTA module (stores cfg). The actual worker is the shared
 * maintenance task in app_main; requests are dispatched to it via cfg.submit. */
esp_err_t ota_update_init(const ota_update_config_t *cfg);

/* Confirm (or roll back) a just-applied PENDING_VERIFY image. Called ONCE by the
 * shared maintenance worker before it serves requests; blocks up to the confirm
 * timeout waiting for MQTT only when an image is actually pending, else returns
 * immediately. Safe to call on a normal boot (no-op). */
void ota_update_run_boot_confirm(void);

/* Queue an OTA from `url` (HTTPS; e.g. a public GitHub release/raw asset). `id`
 * correlates the status reports + the post-reboot confirmation. Non-blocking:
 * the worker suspends comms, downloads, sets boot, reboots. ESP_ERR_INVALID_STATE
 * before init; ESP_ERR_INVALID_ARG on a bad url/id. */
esp_err_t ota_update_request(const char *url, const char *id);

/* Queue a job-driven OTA (see ota_update_job_t). No applied-id dedupe here: the
 * caller decides from ota_update_applied_id_is() whether the job already ran.
 * Skips the fleet jitter, since AWS paces a rollout with its own per-minute cap.
 * On a non-ESP_OK return nothing was queued and no callback will fire. */
esp_err_t ota_update_request_job(const ota_update_job_t *job);

/* True if `id` is the id latched by the last image that was written and set to
 * boot. After a reboot, a job whose id this matches but whose version is not
 * the running one means the new image was rolled back. */
bool ota_update_applied_id_is(const char *id);

/* True from successful queue admission until the OTA job finishes (or reboots
 * on success), capped at 30 minutes for watchdog-veto purposes. Expiry logs one
 * warning and does not clear the real admission state; the general maintenance
 * lock remains the authoritative guard for an operation that is executing. */
bool ota_update_in_progress(void);

#ifdef __cplusplus
}
#endif
