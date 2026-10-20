/* Whole production Jobs + OTA admission + service composition, SDK ports only. */
#define SVC_JOBS_INTEGRATION 1
#define main service_runtime_main
#include "runtime_driver.c"
#undef main
#include "iot_jobs.h"
#include "ota_update.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "nvs.h"

typedef struct { message_received_fn message; message_suback_fn suback; void *context; } subscription_t;
static subscription_t subscriptions[2];
static unsigned subscription_count, job_reports, ota_reports, queries, timer_arms;
static bool timer_armed;
static int64_t timer_due;
static esp_timer_create_args_t timer_args;
static void (*legacy_worker)(void *);
static void *legacy_arg;
static char last_ota[384], last_job[512];

/* Read-only NVS is part of the real OTA applied-id check. Any flash/config
 * write, TLS download or reboot fails the test at the actual SDK boundary. */
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{ assert(!strcmp(ns, "ota_upd") && mode == NVS_READONLY); *out = 1; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *n)
{ (void)h; (void)key; (void)out; (void)n; abort(); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *s)
{ (void)h; (void)key; (void)s; abort(); }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; abort(); }
void nvs_close(nvs_handle_t h) { (void)h; abort(); }
const esp_app_desc_t *esp_app_get_description(void)
{ static const esp_app_desc_t d = {.version = "test-firmware"}; return &d; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "stub-error"; }
void vTaskDelay(TickType_t ticks) { clock_us += (int64_t)ticks * 1000; }
void esp_restart(void) { abort(); }
esp_err_t fleet_jitter_slot_for_sta_mac(uint32_t slots, uint32_t *out)
{ (void)slots; (void)out; abort(); }
const esp_partition_t *esp_ota_get_running_partition(void)
{ static const esp_partition_t p = {.address = 0x10000, .size = 3000000}; return &p; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s)
{ assert(p == esp_ota_get_running_partition()); *s = ESP_OTA_IMG_VALID; return ESP_OK; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { (void)p; abort(); }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t o, void *b, size_t n)
{ (void)p; (void)o; (void)b; (void)n; abort(); }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { abort(); }
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void) { abort(); }
esp_err_t esp_crt_bundle_attach(void *cfg) { (void)cfg; abort(); }
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *c, esp_https_ota_handle_t *h) { (void)c; (void)h; abort(); }
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t h, esp_app_desc_t *d) { (void)h; (void)d; abort(); }
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t h) { (void)h; abort(); }
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t h) { (void)h; abort(); }
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t h) { (void)h; abort(); }
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t h) { (void)h; abort(); }
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t h) { (void)h; abort(); }
void mbedtls_sha256_init(mbedtls_sha256_context *c) { (void)c; abort(); }
int mbedtls_sha256_starts(mbedtls_sha256_context *c, int n) { (void)c; (void)n; abort(); }
int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *b, size_t n) { (void)c; (void)b; (void)n; abort(); }
int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char *b) { (void)c; (void)b; abort(); }
void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; abort(); }

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{ assert(!strcmp(args->name, "jobs_retry")); timer_args = *args; *out = &timer_args; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{ assert(timer == &timer_args); timer_armed = false; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay)
{
    assert(timer == &timer_args && delay == UINT64_C(600000000));
    timer_due = clock_us + (int64_t)delay; timer_armed = true; ++timer_arms; return ESP_OK;
}
static esp_err_t publish(const char *topic, const char *body, size_t n, int *id)
{
    if (id) *id = 1;
    if (!strcmp(topic, "device/status")) {
        assert(n < sizeof last_ota); memcpy(last_ota, body, n); last_ota[n] = 0; ++ota_reports;
    } else if (!strcmp(topic, "$aws/things/AMBYTE_TEST/jobs/$next/get")) {
        assert(n == 2 && !memcmp(body, "{}", 2)); ++queries;
    } else {
        assert(!strcmp(topic, "$aws/things/AMBYTE_TEST/jobs/job-script/update"));
        assert(n < sizeof last_job); memcpy(last_job, body, n); last_job[n] = 0; ++job_reports;
    }
    return ESP_OK;
}
static esp_err_t subscribe(const char *topic, message_received_fn handler, message_suback_fn suback, void *ctx)
{
    assert(subscription_count < 2 && strstr(topic, "$aws/things/AMBYTE_TEST/jobs/") == topic);
    subscriptions[subscription_count++] = (subscription_t){handler, suback, ctx}; return ESP_OK;
}
static void stats(uint32_t *connections, int64_t *age, char *reason, size_t capacity)
{ (void)reason; (void)capacity; *connections = 1; *age = 0; }
static void deliver(const char *operation, const char *id)
{
    char json[512];
    int n = snprintf(json, sizeof json, "{\"execution\":{\"jobId\":\"%s\",\"status\":\"QUEUED\",\"jobDocument\":{\"operation\":\"%s\",\"family\":\"ambyte\",\"version\":\"next\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"url\":\"https://example.test/image.bin\"}}}", id, operation);
    assert(n > 0 && (size_t)n < sizeof json);
    subscriptions[0].message("$aws/things/AMBYTE_TEST/jobs/notify-next", json, (size_t)n, subscriptions[0].context);
}
static bool ota_gate(void) { return !maintenance; }
static void jobs_queued(void (*run)(void *), void *arg)
{
    if (run == ignored_job) return;
    assert(!legacy_worker); legacy_worker = run; legacy_arg = arg;
}
static void jobs_during_trial(void)
{
    assert(usb_service_wifi_busy() && maintenance && !legacy_worker && !ota_update_in_progress());
    deliver("firmware-update", "job-firmware");
    assert(job_reports == 0 && queries == 0 && ota_reports == 1 && timer_arms == 1 && timer_armed);
    assert(!legacy_worker && !ota_update_in_progress());
    assert(!strcmp(last_ota, "{\"type\":\"ota_status\",\"device_id\":\"test\",\"id\":\"job-firmware\",\"state\":\"dropped\",\"fw\":\"test-firmware\",\"detail\":\"an OTA is already in progress\"}"));
    /* A legacy command uses the identical refusal literal/report, not Jobs. */
    assert(ota_update_request("https://example.test/legacy.bin", "legacy") == ESP_ERR_NO_MEM);
    assert(ota_reports == 2 && job_reports == 0 && !legacy_worker && !ota_update_in_progress());
    assert(strstr(last_ota, "\"detail\":\"an OTA is already in progress\"") && strstr(last_ota, "\"id\":\"legacy\""));
    deliver("script-update", "job-script");
    assert(job_reports == 1 && timer_arms == 1);
    assert(!strcmp(last_job, "{\"status\":\"REJECTED\",\"statusDetails\":{\"reason\":\"unsupported operation\",\"runningVersion\":\"v1.0.0\"}}"));
}
int main(int argc, char **argv)
{
    ota_update_config_t ota = {.submit = app_maint_submit_legacy, .publish = publish,
        .status_topic = "device/status", .device_id = "test", .maintenance_begin = ota_gate};
    assert(ota_update_init(&ota) == ESP_OK);
    iot_jobs_config_t jobs = {.thing_name = "AMBYTE_TEST", .family = "ambyte", .running_version = "v1.0.0",
        .publish = publish, .add_subscription = subscribe, .connection_stats = stats};
    assert(iot_jobs_init(&jobs) == ESP_OK && subscription_count == 2);
    subscriptions[0].suback(true, 1, subscriptions[0].context);
    subscriptions[1].suback(true, 1, subscriptions[1].context);
    assert(queries == 1); queries = 0;  /* live granted connection, not a policy-denied no-op */
    assert(service_runtime_main(argc, argv) == 0);
    assert(!usb_service_wifi_busy() && !maintenance && !legacy_worker);
    if (manager_calls) {
        assert(timer_armed && timer_arms == 1);
        /* Fake the timer clock only after the bounded Wi-Fi trial has ended. */
        clock_us = timer_due - 1;
        assert(queries == 0);
        ++clock_us; timer_armed = false; timer_args.callback(timer_args.arg);
        assert(queries == 1);
    } else assert(reject == false && !timer_armed && timer_arms == 0); /* failed service queue admission */
    unsigned reports_before = job_reports;
    deliver("firmware-update", "job-firmware");
    assert(legacy_worker && ota_update_in_progress() && job_reports == reports_before);
    /* Exercise the other existing retryable callback path without ever
     * invoking TLS/flash. The lease filter already admitted this job. */
    maintenance = true;
    legacy_worker(legacy_arg); legacy_worker = NULL; maintenance = false;
    assert(!ota_update_in_progress() && job_reports == reports_before && timer_armed);
    fprintf(stderr, "JOBS_OK queue_untouched=1 retry_us=600000000 reports_during_valid_trial=0 timers=%u\n", timer_arms);
    return 0;
}
