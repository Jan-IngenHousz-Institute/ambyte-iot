#include "sync_runner.h"

#include <time.h>

#include "device_commands.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "sync_runner"

/* Clock-validity floor (2024-01-01 UTC). Below this the system clock was never
 * set (RTC invalid and no NVS flash_time) — publishing would land events in
 * 1970 date partitions in the cloud, so the drain is gated until the clock is
 * plausible. Stores are NOT gated (payload-v2 decision: gate publish only);
 * events queue with whatever ticks they have. */
#define SYNC_CLOCK_FLOOR_S 1704067200LL

/* Stagger the first drain past boot-time noise (Wi-Fi join, TLS, DB boot scan). */
#define SYNC_RUNNER_START_DELAY_MS  10000U
/* Fallback re-check when nothing notifies us — catches power-gate openings
 * (battery→external power, which is not a store event) and any missed wake.
 * Most drains are notification-driven; this is just the safety heartbeat. */
#define SYNC_RUNNER_FALLBACK_MS     30000U
#define SYNC_RUNNER_TASK_NAME    "sync_runner"
/* 8 KiB matches lua_runner. cmd_mqtt_publish_next_event heap-allocates the
 * payload buffer and cJSON tree itself, so the task stack stays light. */
#define SYNC_RUNNER_TASK_STACK   8192
#define SYNC_RUNNER_TASK_PRIO    3   /* below lua_runner (5), above idle */

/* Time to wait between publish attempts while draining, to let the broker's
 * PUBACK free the single in-flight slot before the next publish. */
#define SYNC_RUNNER_ACK_POLL_MS  100U
/* Cap on consecutive "still in flight" polls before abandoning the drain this
 * cycle (~5 s) — guards against a lost PUBACK wedging the loop. */
#define SYNC_RUNNER_MAX_ACK_POLLS 50
/* An in-flight slot held longer than this had its PUBACK lost/expired (or the
 * broker dropped without notice while Wi-Fi stayed up): reap it so the event
 * re-publishes instead of the slot wedging the drain permanently. Well above any
 * real PUBACK round-trip and above esp-mqtt's ~30 s outbox-expiry default. */
#define SYNC_RUNNER_INFLIGHT_MAX_MS 60000

static TaskHandle_t s_task_handle = NULL;
/* STATUS heartbeat period (s); 0 = disabled. Set once at start. */
static uint32_t     s_heartbeat_s = 0;

/* Wake the drain task. Registered as the device_commands store/measurement-end
 * notifier; also safe to call directly. No-op until the task exists. */
void sync_runner_notify(void)
{
    TaskHandle_t h = s_task_handle;
    if (h != NULL) {
        xTaskNotifyGive(h);
    }
}

/* Sync gate: pause publishing while a measurement is in progress so MQTT does
 * not compete with latency-sensitive sensor reads, AND while the device is on
 * battery (Phase 1 power gate) so we only spend the radio budget on external
 * power. Both conditions drain naturally once they clear — events stay PENDING
 * in the event_log DB meanwhile. Weak so a future power_monitor can override. */
__attribute__((weak)) bool sync_runner_is_allowed(void)
{
    return !device_commands_measurement_active() &&
           device_commands_publish_power_ok();
}

/* Publish pending events back-to-back until the queue drains, the gate closes,
 * or a PUBACK stalls. One measure_id = one message, one in flight at a time. */
static void sync_runner_drain(void)
{
    int ack_polls = 0;
    while (sync_runner_is_allowed()) {
        cmd_result_t res = cmd_mqtt_publish_next_event();
        if (res.status == ESP_OK) {
            ESP_LOGI(TAG, "%s", res.message);
            ack_polls = 0;
            vTaskDelay(pdMS_TO_TICKS(SYNC_RUNNER_ACK_POLL_MS));
        } else if (res.status == ESP_ERR_INVALID_STATE) {
            /* Previous event still in flight — normally clears within a PUBACK
             * round-trip. But a lost/expired PUBACK (or a broker drop while
             * Wi-Fi stays associated) can leave the slot latched forever; if it
             * has been held far longer than any real round-trip, reap it so the
             * event re-publishes instead of the drain wedging until reboot. */
            if (device_commands_reap_stale_inflight(SYNC_RUNNER_INFLIGHT_MAX_MS)) {
                ack_polls = 0;
                continue;   /* slot freed — re-claim and publish immediately */
            }
            if (++ack_polls > SYNC_RUNNER_MAX_ACK_POLLS) {
                ESP_LOGW(TAG, "drain stalled waiting for PUBACK");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(SYNC_RUNNER_ACK_POLL_MS));
        } else if (res.status == ESP_ERR_NOT_FOUND) {
            return; /* nothing left to publish */
        } else {
            /* NOT_SUPPORTED (no MQTT/persistence) or a publish error. */
            if (res.status != ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "publish skipped: %s", res.message);
            }
            return;
        }
    }
}

static void sync_runner_task(void *arg)
{
    (void)arg;

    /* Stagger the first run so we don't compete with boot-time noise
     * (Wi-Fi join, MQTT TLS handshake, event_log boot scan). */
    vTaskDelay(pdMS_TO_TICKS(SYNC_RUNNER_START_DELAY_MS));

    /* Heartbeat bookkeeping: backdated so the first pass stores immediately
     * (a boot marker in the STATUS stream). Tick math is wrap-safe. */
    const TickType_t hb_ticks = pdMS_TO_TICKS(s_heartbeat_s * 1000U);
    TickType_t last_hb        = xTaskGetTickCount() - hb_ticks;
    bool clock_warned         = false;

    while (1) {
        /* Sleep until something stores an event / a measurement burst ends
         * (sync_runner_notify), or the fallback timer fires. pdTRUE clears the
         * notification count on take, so a burst of N stores collapses into one
         * drain pass. The CPU can idle in between — no fixed-rate polling. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYNC_RUNNER_FALLBACK_MS));

        /* STATUS heartbeat (payload-v2 Phase 4): firmware-owned so a broken or
         * missing main.lua can't silence status reporting. Resolution is the
         * fallback period (±30 s on the 5 min default) — fine for telemetry. */
        if (s_heartbeat_s > 0) {
            TickType_t now_tick = xTaskGetTickCount();
            if ((now_tick - last_hb) >= hb_ticks) {
                cmd_result_t hr = cmd_store_status_event();
                if (hr.status == ESP_OK) {
                    ESP_LOGI(TAG, "%s", hr.message);
                } else if (hr.status != ESP_ERR_NOT_SUPPORTED) {
                    ESP_LOGW(TAG, "heartbeat: %s", hr.message);
                }
                last_hb = now_tick;
            }
        }

        /* Clock gate: never publish 1970-stamped events (see SYNC_CLOCK_FLOOR_S).
         * Logged on state change only. */
        if (time(NULL) < (time_t)SYNC_CLOCK_FLOOR_S) {
            if (!clock_warned) {
                ESP_LOGW(TAG, "system clock unset (pre-2024) — publishing gated "
                              "until the RTC/flash-time sets it");
                clock_warned = true;
            }
            continue;
        }
        if (clock_warned) {
            ESP_LOGI(TAG, "system clock now valid — publishing resumes");
            clock_warned = false;
        }

        if (sync_runner_is_allowed()) {
            /* Burst-drain all pending events (one msg per id). */
            sync_runner_drain();
        } else {
            ESP_LOGD(TAG, "sync gate closed (measurement active or on battery) — deferring");
        }
    }
}

esp_err_t sync_runner_start(uint32_t heartbeat_s)
{
    if (s_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_heartbeat_s = heartbeat_s;

    BaseType_t ok = xTaskCreate(
        sync_runner_task,
        SYNC_RUNNER_TASK_NAME,
        SYNC_RUNNER_TASK_STACK,
        NULL,
        SYNC_RUNNER_TASK_PRIO,
        &s_task_handle
    );
    if (ok != pdPASS) {
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Become the wake target for every stored event + measurement-end. */
    device_commands_set_sync_notifier(sync_runner_notify);
    ESP_LOGI(TAG, "background sync started (wake-on-store, fallback=%u ms, heartbeat=%u s)",
             (unsigned)SYNC_RUNNER_FALLBACK_MS, (unsigned)heartbeat_s);
    return ESP_OK;
}
