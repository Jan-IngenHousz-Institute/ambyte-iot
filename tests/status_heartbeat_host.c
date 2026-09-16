#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "status_heartbeat.h"
#include "freertos/task.h"

static const char *scenario;
static int64_t now_ms;
static int attempts, successes;
static int64_t sent_at[8];
static void (*task_fn)(void *);
static jmp_buf done;
static bool is(const char *s) { return strcmp(scenario, s) == 0; }
int64_t esp_timer_get_time(void) { return now_ms * 1000; }
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
    if (now_ms > 1800000) longjmp(done, 1);
}
static bool wifi(void) { return !is("wifi_down"); }
static bool mqtt(void)
{
    return !is("mqtt_down") && (!is("reconnect") || now_ms >= 1200000);
}
static bool sd(void) { return false; } /* parked/absent for the entire test */
static esp_err_t power(power_reading_t *p)
{
    *p = (power_reading_t){.battery_mv = is("critical") ? 3100 : 3800,
                         .input_present = is("charging"), .charge_status = is("charging") ? 2 : 0};
    return is("power_error") ? ESP_FAIL : ESP_OK;
}
static void *alloc(size_t size)
{
    if (is("allocation_failure") && now_ms < 30000) return NULL;
    return malloc(size);
}
static esp_err_t publish(const char *topic, const char *payload, size_t len, int *id)
{
    assert(wifi() && mqtt());
    assert(strcmp(topic, "test/device/status") == 0);
    assert(len == strlen(payload));
    attempts++;
    cJSON *j = cJSON_Parse(payload);
    assert(j);
    assert(strcmp(cJSON_GetObjectItem(j,"type")->valuestring,"heartbeat") == 0);
    assert(strcmp(cJSON_GetObjectItem(j,"device_id")->valuestring,"AMBYTE_\"test") == 0);
    assert(strcmp(cJSON_GetObjectItem(j,"fw")->valuestring,"2.2.1") == 0);
    assert(cJSON_GetObjectItem(j,"uptime_ms")->valuedouble == (double)now_ms);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(j,"sd_ready")));
    cJSON *p = cJSON_GetObjectItem(j,"power");
    if (is("power_error")) assert(cJSON_IsNull(p));
    else {
        assert(cJSON_GetObjectItem(p,"battery_mv")->valueint == (is("critical") ? 3100 : 3800));
        assert(cJSON_IsTrue(cJSON_GetObjectItem(p,"input_present")) == is("charging"));
    }
    cJSON_Delete(j);
    if (is("publish_failure") && attempts == 1) return ESP_FAIL;
    *id = attempts;
    assert(successes < 8);
    sent_at[successes++] = now_ms;
    return ESP_OK;
}
int main(int argc, char **argv)
{
    assert(argc == 2); scenario = argv[1];
    cJSON_Hooks hooks = {.malloc_fn=alloc,.free_fn=free}; cJSON_InitHooks(&hooks);
    status_heartbeat_config_t cfg = {
        .publish=publish,.mqtt_connected=mqtt,.wifi_connected=wifi,.read_power=power,
        .sd_ready=sd,.status_topic="test/device/status",.device_id="AMBYTE_\"test",.firmware_version="2.2.1",
    };
    assert(status_heartbeat_start(NULL) == ESP_ERR_INVALID_ARG);
    assert(status_heartbeat_start(&cfg) == ESP_OK);
    assert(status_heartbeat_start(&cfg) == ESP_ERR_INVALID_STATE);
    if (setjmp(done) == 0) task_fn(NULL);
    if (is("wifi_down") || is("mqtt_down")) assert(attempts == 0);
    else if (is("reconnect")) { assert(successes == 1); assert(sent_at[0] == 1200000); }
    else if (is("publish_failure") || is("allocation_failure")) {
        assert(successes == 2); assert(sent_at[0] == 30000); assert(sent_at[1] == 930000);
    } else {
        assert(successes == 3); assert(sent_at[0] == 0);
        assert(sent_at[1] == 900000); assert(sent_at[2] == 1800000);
    }
    printf("heartbeat %s: ok\n", scenario);
    return 0;
}
