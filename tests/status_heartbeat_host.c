#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "status_heartbeat.h"
#include "freertos/task.h"

static const char *scenario;
static int64_t now_ms;
static int attempts, successes;
static int64_t sent_at[16];
static void (*task_fn)(void *);
static jmp_buf done;
static bool is(const char *s) { return strcmp(scenario, s) == 0; }
static bool churn(void) { return is("churn450") || is("churn899"); }
int64_t esp_timer_get_time(void) { return now_ms * 1000; }
esp_err_t fleet_jitter_slot_for_sta_mac(uint32_t slots, uint32_t *out)
{
    assert(slots == 900);
    *out = (is("jitter") || is("reconnect_jitter") || is("brief_reconnect")) ? 137 : 0;
    if (churn()) *out = is("churn450") ? 450 : 899;
    return ESP_OK;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)arg; (void)priority;
    task_fn = fn; *handle = (void *)1; return pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0 && ticks <= 30000);
    now_ms += ticks;
    if (now_ms > (churn() ? 3600000 : 1800000)) longjmp(done, 1);
}
static bool wifi(void) { return !is("wifi_down"); }
static bool mqtt(void)
{
    if (churn()) return now_ms % 600000 < 240000; /* 4 min up / 6 min down */
    if (is("brief_reconnect") && now_ms >= 90000 && now_ms < 110000) return false;
    return !is("mqtt_down") && (!(is("reconnect") || is("reconnect_jitter")) || now_ms >= 1200000);
}
static bool allowed(void) { return !is("sensor_hold") || now_ms >= 5000; }
static esp_err_t publish(void)
{
    assert(wifi() && mqtt() && allowed());
    attempts++;
    if (is("allocation_failure") && attempts == 1) return ESP_ERR_NO_MEM;
    if (is("publish_failure") && attempts == 1) return ESP_FAIL;
    assert(successes < 16);
    sent_at[successes++] = now_ms;
    return ESP_OK;
}
int main(int argc, char **argv)
{
    assert(argc == 2); scenario = argv[1];
    status_heartbeat_config_t cfg = {
        .publish_snapshot=publish,.mqtt_connected=mqtt,.wifi_connected=wifi,
        .publish_allowed=allowed,
    };
    assert(status_heartbeat_start(NULL) == ESP_ERR_INVALID_ARG);
    assert(status_heartbeat_start(&cfg) == ESP_OK);
    assert(status_heartbeat_start(&cfg) == ESP_ERR_INVALID_STATE);
    if (setjmp(done) == 0) task_fn(NULL);
    if (is("wifi_down") || is("mqtt_down")) assert(attempts == 0);
    else if (churn()) { assert(successes >= 3); assert(sent_at[0] <= 1200000); }
    else if (is("reconnect")) { assert(successes == 2); assert(sent_at[0] == 1200000); assert(sent_at[1] == 1800000); }
    else if (is("reconnect_jitter")) { assert(successes == 1); assert(sent_at[0] == 1200000); }
    else if (is("jitter") || is("brief_reconnect")) { assert(successes == 2); assert(sent_at[0] == 137000); assert(sent_at[1] == 1037000); }
    else if (is("sensor_hold")) { assert(successes == 3); assert(sent_at[0] == 5000); assert(sent_at[1] == 900000); }
    else if (is("publish_failure") || is("allocation_failure")) {
        assert(successes == 3); assert(sent_at[0] == 30000); assert(sent_at[1] == 900000);
    } else {
        assert(successes == 3); assert(sent_at[0] == 0);
        assert(sent_at[1] == 900000); assert(sent_at[2] == 1800000);
    }
    printf("heartbeat %s: ok\n", scenario);
    return 0;
}
