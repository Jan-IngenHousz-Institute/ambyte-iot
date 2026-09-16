#include "status_heartbeat.h"

#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HEARTBEAT_INTERVAL_MS (15LL * 60 * 1000)
#define HEARTBEAT_POLL_MS 30000U
#define HEARTBEAT_STACK 4096

static status_heartbeat_config_t s_cfg;
static TaskHandle_t s_task;

static esp_err_t publish_heartbeat(int64_t uptime_ms)
{
    /* Do not reuse cmd_store_status_event: that health event is durable, but
     * sits behind the power-gated FIFO. In the field it makes a Wi-Fi-connected
     * battery unit appear dead all night. This small operational reply uses
     * the already-authorized command-status topic, exactly like a pong, and
     * neither drains nor advances the measurement queue. */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    bool ok = cJSON_AddStringToObject(root, "type", "heartbeat") != NULL &&
              cJSON_AddStringToObject(root, "device_id", s_cfg.device_id) != NULL &&
              cJSON_AddStringToObject(root, "fw", s_cfg.firmware_version) != NULL &&
              cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms) != NULL &&
              cJSON_AddBoolToObject(root, "wifi", true) != NULL &&
              cJSON_AddBoolToObject(root, "mqtt", true) != NULL;
    if (ok) {
        ok = (s_cfg.sd_ready != NULL
            ? cJSON_AddBoolToObject(root, "sd_ready", s_cfg.sd_ready())
            : cJSON_AddNullToObject(root, "sd_ready")) != NULL;
    }

    power_reading_t power = {0};
    if (ok && s_cfg.read_power != NULL && s_cfg.read_power(&power) == ESP_OK) {
        cJSON *p = cJSON_AddObjectToObject(root, "power");
        ok = p != NULL &&
             cJSON_AddNumberToObject(p, "battery_mv", power.battery_mv) != NULL &&
             cJSON_AddNumberToObject(p, "input_mv", power.input_mv) != NULL &&
             cJSON_AddBoolToObject(p, "input_present", power.input_present) != NULL &&
             cJSON_AddNumberToObject(p, "charge_ma", power.charge_ma) != NULL &&
             cJSON_AddNumberToObject(p, "charge_status", power.charge_status) != NULL;
    } else if (ok) {
        ok = cJSON_AddNullToObject(root, "power") != NULL;
    }
    char *payload = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (payload == NULL) return ESP_ERR_NO_MEM;
    int msg_id = 0;
    esp_err_t err = s_cfg.publish(s_cfg.status_topic, payload, strlen(payload), &msg_id);
    cJSON_free(payload);
    return err;
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    int64_t next_due_ms = 0; /* first report as soon as Wi-Fi + MQTT are ready */
    for (;;) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= next_due_ms && s_cfg.wifi_connected() && s_cfg.mqtt_connected()) {
            if (publish_heartbeat(now_ms) == ESP_OK) {
                next_due_ms = now_ms + HEARTBEAT_INTERVAL_MS;
            } else {
                ESP_LOGW("heartbeat", "status submission failed; retry in 30 s");
            }
        }
        /* A failed submission or an offline interval never consumes the due
         * report. Reconnection sends ONE fresh snapshot, not a night's backlog.
         * Monotonic uptime keeps this alive with an unset/jumping wall clock. */
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_POLL_MS));
    }
}

esp_err_t status_heartbeat_start(const status_heartbeat_config_t *config)
{
    if (config == NULL || config->publish == NULL || config->wifi_connected == NULL ||
        config->mqtt_connected == NULL || config->status_topic == NULL ||
        config->status_topic[0] == '\0' || config->device_id == NULL ||
        config->firmware_version == NULL) return ESP_ERR_INVALID_ARG;
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_cfg = *config;
    if (xTaskCreate(heartbeat_task, "status_hb", HEARTBEAT_STACK, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
