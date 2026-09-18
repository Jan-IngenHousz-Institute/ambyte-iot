#include "status_heartbeat.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "fleet_jitter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HEARTBEAT_INTERVAL_MS (15LL * 60 * 1000)
#define HEARTBEAT_POLL_MS 30000U
#define HEARTBEAT_STACK 10240

static status_heartbeat_config_t s_cfg;
static TaskHandle_t s_task;

static int64_t next_phase_after(int64_t now_ms, int64_t phase_ms)
{
    int64_t elapsed = (now_ms - phase_ms) % HEARTBEAT_INTERVAL_MS;
    if (elapsed < 0) elapsed += HEARTBEAT_INTERVAL_MS;
    return now_ms + HEARTBEAT_INTERVAL_MS - elapsed;
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t slot = 0;
    if (fleet_jitter_slot_for_sta_mac(900, &slot) != ESP_OK) {
        ESP_LOGW("heartbeat", "MAC jitter unavailable; using slot 0");
    }
    const int64_t phase_ms = (int64_t)slot * 1000;
    int64_t next_due_ms = phase_ms;
    for (;;) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const bool connected = s_cfg.wifi_connected() && s_cfg.mqtt_connected();
        uint32_t sleep_ms = HEARTBEAT_POLL_MS;
        if (connected) {
            if (now_ms >= next_due_ms) {
                if (s_cfg.publish_allowed != NULL && !s_cfg.publish_allowed()) {
                    /* Keep the raw-sensor hold (including the legacy full-cycle
                     * rollback), but poll finely: 30 s sampling can phase-lock
                     * with a repeated measurement and starve every report. */
                    sleep_ms = 1000;
                } else if (s_cfg.publish_snapshot() == ESP_OK) {
                    /* Return to this device's phase on the uptime grid. Never
                     * push an overdue deadline forward on reconnection: links
                     * with up-times shorter than the MAC slot would otherwise
                     * suppress reports forever. A recovery snapshot can be
                     * followed by the next grid report in less than 15 min. */
                    next_due_ms = next_phase_after(now_ms, phase_ms);
                } else {
                    ESP_LOGW("heartbeat", "status submission failed; retry in 30 s");
                }
            }
            const int64_t remaining_ms = next_due_ms - esp_timer_get_time() / 1000;
            if (remaining_ms > 0 && remaining_ms < sleep_ms) {
                sleep_ms = (uint32_t)remaining_ms;
            }
        }
        /* A failed submission or an offline interval never consumes the due
         * report. Reconnection sends ONE fresh snapshot, not a night's backlog.
         * Monotonic uptime keeps this alive with an unset/jumping wall clock. */
        vTaskDelay(pdMS_TO_TICKS(sleep_ms) > 0 ? pdMS_TO_TICKS(sleep_ms) : 1);
    }
}

esp_err_t status_heartbeat_start(const status_heartbeat_config_t *config)
{
    if (config == NULL || config->publish_snapshot == NULL ||
        config->wifi_connected == NULL || config->mqtt_connected == NULL)
        return ESP_ERR_INVALID_ARG;
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_cfg = *config;
    if (xTaskCreate(heartbeat_task, "status_hb", HEARTBEAT_STACK, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
