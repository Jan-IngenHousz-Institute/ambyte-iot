#include "usb_service.h"
#include "svc_engine.h"
#include "svc_session.h"
#include "svc_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sensor_inventory.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "script_update.h"
#include "wifi_manager.h"

static svc_engine_t s_engine;
static svc_store_t s_store;
static usb_service_config_t s_config;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_cli_task;
static svc_metrics_t s_metrics;
static portMUX_TYPE s_metrics_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned char s_boot_fingerprint[32];
static bool s_boot_fingerprint_known;
static bool s_initialized;
static sensor_snapshot_t *s_sensor_pin;
static uint64_t s_sensor_token;
static int64_t s_sensor_pinned_ms;
static size_t s_sensor_size;

static void lock(void *context) { (void)context; xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void *context) { (void)context; xSemaphoreGive(s_mutex); }
static bool ready(void *context) { (void)context; return s_config.boot_ready && s_config.boot_ready(); }
static int64_t now(void *context) { (void)context; return esp_timer_get_time() / 1000; }
static size_t free_bytes(void *context)
{
    (void)context; size_t total = 0, used = 0;
    return esp_littlefs_info("littlefs", &total, &used) == ESP_OK && total >= used ? total - used : 0;
}
static void sample(void *context)
{
    (void)context;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    uint32_t free = heap_caps_get_free_size(caps), largest = heap_caps_get_largest_free_block(caps);
    uint32_t min = heap_caps_get_minimum_free_size(caps);
    portENTER_CRITICAL(&s_metrics_mux); TaskHandle_t cli_task = s_cli_task; portEXIT_CRITICAL(&s_metrics_mux);
    uint32_t cli = cli_task ? uxTaskGetStackHighWaterMark(cli_task) * sizeof(StackType_t) : 0;
    uint32_t worker = s_config.worker_stack ? s_config.worker_stack() : 0;
    uint32_t runner = s_config.runner_stack ? s_config.runner_stack() : 0;
    portENTER_CRITICAL(&s_metrics_mux);
    s_metrics.free_heap = free; s_metrics.largest_block = largest; s_metrics.min_heap = min;
    if (!s_metrics.samples || free < s_metrics.min_free_heap) s_metrics.min_free_heap = free;
    if (!s_metrics.samples || largest < s_metrics.min_largest_block) s_metrics.min_largest_block = largest;
    if (s_metrics.samples != UINT32_MAX) ++s_metrics.samples;
    if (runner && (!s_metrics.runner_stack || runner < s_metrics.runner_stack)) s_metrics.runner_stack = runner;
    if (cli && (!s_metrics.cli_stack || cli < s_metrics.cli_stack)) s_metrics.cli_stack = cli;
    if (worker && (!s_metrics.worker_stack || worker < s_metrics.worker_stack)) s_metrics.worker_stack = worker;
    portEXIT_CRITICAL(&s_metrics_mux);
}
/* Strong implementation of the sched_spec optional observer. It samples no
 * filesystem and takes no service/maintenance mutex, so parser callbacks are
 * safe inside synchronous schedule validation and runner restart. */
void sched_resource_sample(void) { sample(NULL); }

static void metrics(void *context, svc_metrics_t *out)
{
    sample(context);
    portENTER_CRITICAL(&s_metrics_mux); *out = s_metrics; portEXIT_CRITICAL(&s_metrics_mux);
    svc_usb_counters(&out->dropped_calls, &out->dropped_bytes);
    size_t total = 0, used = 0;
    if (esp_littlefs_info("littlefs", &total, &used) == ESP_OK) {
        out->fs_total = (uint32_t)total; out->fs_used = (uint32_t)used;
    }
}
static bool sha256(void *context, const void *bytes, size_t n, unsigned char out[32])
{
    sample(context);
    return mbedtls_sha256(bytes, n, out, 0) == 0;
}
typedef struct { mbedtls_sha256_context hash; size_t size; } sensor_digest_t;
static bool sensor_digest(void *context, const void *bytes, size_t size)
{
    sensor_digest_t *digest = context; digest->size += size;
    return mbedtls_sha256_update(&digest->hash, bytes, size) == 0;
}
static void sensors_release(void *context)
{
    sample(context);
    free(s_sensor_pin); s_sensor_pin = NULL; s_sensor_size = 0;
    sensor_inventory_note_pin(0); sample(context);
}
static const char *sensors_capture(void *context, uint64_t *token, size_t *size, char sha[65])
{
    if (!sensor_inventory_ready()) return "sensors_unavailable";
    sensors_release(context);
    uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_sensor_pin = malloc(sizeof *s_sensor_pin);
    sample(context);
    if (!s_sensor_pin) return "no_memory";
    uint32_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t charge = heap_caps_get_allocated_size(s_sensor_pin);
    if (before > after && before - after > charge) charge = before - after;
    sensor_inventory_note_pin((uint32_t)charge);
    if (!sensor_inventory_snapshot(s_sensor_pin)) { sensors_release(context); return "sensors_unavailable"; }
    sensor_digest_t digest = {0}; unsigned char hash[32];
    mbedtls_sha256_init(&digest.hash);
    bool ok = mbedtls_sha256_starts(&digest.hash, 0) == 0 &&
        sensor_snapshot_json(s_sensor_pin, sensor_digest, &digest) && mbedtls_sha256_finish(&digest.hash, hash) == 0;
    mbedtls_sha256_free(&digest.hash);
    if (!ok || !digest.size || digest.size > SENSOR_SNAPSHOT_MAX) { sensors_release(context); return "sensors_unavailable"; }
    if (!s_sensor_token) do esp_fill_random(&s_sensor_token, sizeof s_sensor_token); while (!s_sensor_token);
    if (s_sensor_token == UINT64_MAX) { sensors_release(context); return "sensors_unavailable"; }
    ++s_sensor_token; *token = s_sensor_token; *size = s_sensor_size = digest.size;
    s_sensor_pinned_ms = now(NULL);
    for (unsigned i = 0; i < 32; ++i) snprintf(sha + 2*i, 3, "%02x", hash[i]);
    sample(context); return NULL;
}
static const char *sensors_read(void *context, uint64_t token, uint32_t offset,
                                unsigned char data[576], size_t *size, bool *eof)
{
    if (!s_sensor_pin || token != s_sensor_token) return "snapshot_expired";
    if (now(NULL) - s_sensor_pinned_ms >= 60000) { sensors_release(context); return "snapshot_expired"; }
    if (offset >= s_sensor_size) return "invalid_args";
    *size = sensor_snapshot_slice(s_sensor_pin, offset, data, 576, NULL);
    if (!*size) return "sensors_unavailable";
    *eof = offset + *size == s_sensor_size;
    sample(context); return NULL;
}
static bool submit(void *context, void (*run)(void *), void *arg)
{
    (void)context; return s_config.submit && s_config.submit(run, arg);
}
static bool maintenance_begin(void *context)
{
    (void)context; return s_config.maintenance_begin && s_config.maintenance_begin();
}
static void maintenance_end(void *context) { (void)context; s_config.maintenance_end(); }
static script_service_descriptor_t descriptor(const svc_record_t *r)
{
    return (script_service_descriptor_t){.source_path = SCRIPT_UPDATE_SERVICE_STAGING_PATH,
        .size = r->size, .sha256 = r->sha256, .workbook = r->workbook, .version = r->version, .built_fw = r->built_fw};
}
static bool schedule_matches(void *context, const svc_record_t *r)
{
    (void)context; script_service_descriptor_t d = descriptor(r);
    return script_update_service_matches(&d);
}
static const char *schedule_apply(void *context, const svc_record_t *r, svc_truth_t *applied)
{
    sample(context);
    script_service_descriptor_t d = descriptor(r);
    bool changed = false;
    const char *error = script_update_service_apply(&d, &changed);
    *applied = changed ? SVC_TRUE : SVC_FALSE;
    sample(context);
    return error;
}
static bool wifi_status(void *context, svc_wifi_status_t *out)
{
    (void)context; wifi_manager_service_status_t status;
    if (wifi_manager_service_status(&status) != ESP_OK) return false;
    memcpy(out->configured_ssid, status.configured_ssid, sizeof out->configured_ssid);
    memcpy(out->trial_ssid, status.trial_ssid, sizeof out->trial_ssid);
    out->trial = status.trial; out->connected = status.connected; out->provisioned = status.provisioned;
    return true;
}
static const char *wifi_reason(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return NULL;
    case WIFI_MANAGER_ERR_AUTH_REJECTED: return "wifi_auth_rejected";
    case WIFI_MANAGER_ERR_NOT_RETRYING: return "wifi_not_retrying";
    case WIFI_MANAGER_ERR_DRIVER_UNRESOLVED: return "wifi_driver_unresolved";
    case ESP_ERR_TIMEOUT: return "wifi_timeout";
    case ESP_ERR_WIFI_PASSWORD: case ESP_ERR_INVALID_ARG: return "wifi_invalid_credentials";
    case ESP_ERR_INVALID_STATE: case ESP_ERR_WIFI_NOT_INIT: case ESP_ERR_WIFI_NOT_STARTED: return "wifi_unavailable";
    default: return "wifi_failed";
    }
}
static void wifi_trial(void *context, const char *ssid, const char *password,
                        svc_prepare_fn prepare, void *prepare_context, svc_trial_result_t *out)
{
    sample(context);
    wifi_manager_trial_result_t result;
    esp_err_t error = wifi_manager_trial(ssid, password, prepare, prepare_context, &result);
    out->reason = wifi_reason(error); out->saved = result.saved; out->connected = result.connected;
    out->retrying = result.retrying; out->restored = result.restored; out->persist_intent = result.persist_intent;
    svc_wipe(&result, sizeof result);
    sample(context);
}
static bool boot_fingerprint(void *context, unsigned char out[32])
{
    (void)context; memcpy(out, s_boot_fingerprint, 32); return s_boot_fingerprint_known;
}
static bool enter(void *context) { (void)context; return svc_usb_enter(); }
static void leave(void *context) { (void)context; svc_usb_leave(); }
static int read_bytes(void *context, void *bytes, size_t n, unsigned wait)
{ (void)context; return svc_usb_read(bytes, n, wait); }
static bool write_bytes(void *context, const void *bytes, size_t n)
{ (void)context; return svc_usb_write(bytes, n); }
static bool overrun(void *context) { (void)context; return svc_usb_take_overrun(); }
static void notice(const char *reason)
{
    char json[SVC_JSON_MAX + 1], frame[SVC_FRAME_MAX + 1];
    svc_protocol_error(&s_engine.protocol, reason, json);
    size_t n = svc_frame(frame, sizeof frame, 0, json);
    if (n) (void)svc_usb_notice_write(frame, n);
    svc_wipe(json, sizeof json); svc_wipe(frame, sizeof frame);
}
esp_err_t usb_service_init(const usb_service_config_t *config)
{
    if (!config || !config->submit || !config->maintenance_begin || !config->maintenance_end || !config->boot_ready)
        return ESP_ERR_INVALID_ARG;
    if (s_mutex) return ESP_ERR_INVALID_STATE;
    s_config = *config;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    uint64_t fresh;
    do esp_fill_random(&fresh, sizeof fresh); while (!fresh);
    const char *error = svc_store_open(&s_store, "/littlefs/svc", fresh, free_bytes, NULL);
    if (error) s_store.corrupt = true;
    svc_engine_ports_t ports = {.protocol = {.sha256 = sha256, .metrics = metrics, .wifi_status = wifi_status,
        .sensors_capture = sensors_capture, .sensors_read = sensors_read, .sensors_release = sensors_release},
        .lock = lock, .unlock = unlock, .submit = submit, .maintenance_begin = maintenance_begin,
        .maintenance_end = maintenance_end, .boot_ready = ready, .now_ms = now,
        .schedule_apply = schedule_apply, .schedule_matches = schedule_matches,
        .wifi_trial = wifi_trial, .wifi_current_fingerprint = boot_fingerprint};
    svc_engine_init(&s_engine, &s_store, &ports);
    wifi_config_t boot = {0}; char ssid[33] = {0}, password[65] = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &boot) == ESP_OK) {
        memcpy(ssid, boot.sta.ssid, sizeof boot.sta.ssid);
        memcpy(password, boot.sta.password, sizeof boot.sta.password);
        s_boot_fingerprint_known = svc_wifi_fingerprint(&s_engine.protocol.ports, ssid, password, s_boot_fingerprint);
    }
    svc_wipe(&boot, sizeof boot); svc_wipe(ssid, sizeof ssid); svc_wipe(password, sizeof password);
    s_initialized = true;
    svc_usb_notice_handler(notice);
    return error ? ESP_FAIL : ESP_OK;
}
void usb_service_boot_ready(void) { if (s_initialized) (void)svc_engine_tick(&s_engine); }
bool usb_service_wifi_busy(void) { return s_initialized && svc_engine_wifi_busy(&s_engine); }
int usb_service_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) { printf("Usage: svc\r\n"); return 1; }
    if (!s_initialized) { printf("svc: unavailable\r\n"); return 1; }
    portENTER_CRITICAL(&s_metrics_mux); s_cli_task = xTaskGetCurrentTaskHandle(); portEXIT_CRITICAL(&s_metrics_mux);
    const svc_session_ports_t ports = {.enter = enter, .leave = leave, .read = read_bytes,
        .write = write_bytes, .overrun = overrun, .now_ms = now, .sample = sample};
    /* Never let the REPL print a command-error marker after a failed frame
     * emission. The client detects the transport loss and recovers by ID. */
    (void)svc_session_run(&s_engine, &ports);
    return 0;
}
