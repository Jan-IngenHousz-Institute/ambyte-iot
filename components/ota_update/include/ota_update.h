#pragma once

#include "esp_err.h"
#include "messaging_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MQTT-triggered self-OTA for the ambyte (docs/ota-update-plan.md, Stage 3).
 *
 * Triggered today by the custom command topic (command_router dispatches an
 * `ota_update` command here); a later swap moves the trigger to AWS IoT Jobs
 * without touching this handler. The download is Stage-0-proven `esp_https_ota`
 * from a public HTTPS URL (e.g. a GitHub release asset).
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
    void                  (*workload_suspend)(void); /* stop the Lua measurement task during the DL
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
    const char             *status_topic;   /* where status JSON is published */
    const char             *device_id;      /* included in status payloads */
} ota_update_config_t;

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

#ifdef __cplusplus
}
#endif
