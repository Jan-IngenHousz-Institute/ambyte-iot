/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../components/usb_service/usb_service.c"

static unsigned critical, manager_calls, raw_submissions, legacy_submissions, maintenance_ends;
static bool mutex_held, gate, maintenance, reject;
static int64_t clock_us;
static void (*pending_worker)(void *);
static void *worker_arg;
static char input[2048];
static size_t input_at, input_length;
static const char *case_name;
#ifdef SVC_JOBS_INTEGRATION
static void jobs_during_trial(void);
static void jobs_queued(void (*run)(void *), void *arg);
#endif

void svc_test_enter(void) { assert(critical++ == 0); }
void svc_test_exit(void) { assert(critical-- == 1); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait)
{ assert(mutex == (void *)1 && wait == portMAX_DELAY && !mutex_held); mutex_held = true; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(mutex == (void *)1 && mutex_held); mutex_held = false; return pdTRUE; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { assert(task == (void *)1); return 8888; }
int64_t esp_timer_get_time(void) { clock_us += 1000; return clock_us; }
size_t heap_caps_get_free_size(unsigned caps) { assert(caps == 3); return 64000; }
size_t heap_caps_get_largest_free_block(unsigned caps) { assert(caps == 3); return 32000; }
size_t heap_caps_get_minimum_free_size(unsigned caps) { assert(caps == 3); return 60000; }
esp_err_t esp_littlefs_info(const char *partition, size_t *total, size_t *used)
{ assert(!strcmp(partition, "littlefs")); *total = 1024 * 1024; *used = 1024; return ESP_OK; }
void esp_fill_random(void *out, size_t length)
{ assert(length == 8); uint64_t id = UINT64_C(0x0123456789abcdef); memcpy(out, &id, length); }
int mbedtls_sha256(const unsigned char *bytes, size_t n, unsigned char out[32], int is224)
{ assert(!is224); unsigned length; return EVP_Digest(bytes, n, out, &length, EVP_sha256(), NULL) == 1 && length == 32 ? 0 : -1; }
esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t *config)
{ assert(interface == WIFI_IF_STA); memset(config, 0, sizeof *config); strcpy((char *)config->sta.ssid, "original"); strcpy((char *)config->sta.password, "original-password"); return ESP_OK; }
esp_err_t wifi_manager_service_status(wifi_manager_service_status_t *out)
{ memset(out, 0, sizeof *out); strcpy(out->configured_ssid, "original"); out->connected = out->provisioned = true; return ESP_OK; }

static bool app_maint_submit(void (*run)(void *), void *arg)
{
    if (reject) return false;
    if (arg == &s_engine) { assert(!pending_worker); ++raw_submissions; pending_worker = run; worker_arg = arg; }
    else {
        ++legacy_submissions;
#ifdef SVC_JOBS_INTEGRATION
        jobs_queued(run, arg);
#endif
    }
    return true;
}
#include "../../main/service_admission.h"
static bool backend_begin(void) { assert(!maintenance); maintenance = true; return true; }
static void backend_end(void) { assert(maintenance); maintenance = false; ++maintenance_ends; }
static bool backend_ready(void) { return true; }
static uint32_t backend_stack(void) { return 4444; }
static void ignored_job(void *arg) { (void)arg; abort(); }
esp_err_t wifi_manager_trial(const char *ssid, const char *password, wifi_manager_trial_prepare_fn prepare,
                             void *context, wifi_manager_trial_result_t *out)
{
    assert(!mutex_held && maintenance && !strcmp(ssid, "A3Bench") && !strcmp(password, "wrongpass"));
    ++manager_calls; memset(out, 0, sizeof *out);
    assert(usb_service_wifi_busy());
    /* All three component ports use this same real composition predicate. */
    for (unsigned i = 0; i < 3; ++i) assert(!app_maint_submit_legacy(ignored_job, NULL));
#ifdef SVC_JOBS_INTEGRATION
    jobs_during_trial();
#endif
    assert(prepare(false, "original", "original-password", context));
    if (strcmp(case_name, "success")) {
        out->restored = true; out->connected = true;
        out->result = !strcmp(case_name, "timeout") ? ESP_ERR_TIMEOUT : WIFI_MANAGER_ERR_AUTH_REJECTED;
        return out->result;
    }
    assert(prepare(true, NULL, NULL, context));
    out->persist_intent = out->saved = out->connected = true;
    return ESP_OK;
}
const char *script_update_service_apply(const script_service_descriptor_t *d, bool *applied)
{ (void)d; (void)applied; abort(); }
bool script_update_service_matches(const script_service_descriptor_t *d) { (void)d; return false; }
void svc_usb_notice_handler(void (*handler)(const char *)) { assert(handler == notice); }
bool svc_usb_enter(void) { assert(!gate); gate = true; return true; }
void svc_usb_leave(void) { assert(gate); gate = false; }
bool svc_usb_take_overrun(void) { return false; }
void svc_usb_counters(uint32_t *calls, uint32_t *bytes) { *calls = 12; *bytes = 123; }
int svc_usb_read(void *out, size_t maximum, unsigned wait)
{
    assert(gate);
    size_t n = input_length - input_at;
    if (n > maximum) n = maximum;
    if (n) { memcpy(out, input + input_at, n); input_at += n; }
    else clock_us += wait * 1000;
    return (int)n;
}
bool svc_usb_write(const void *bytes, size_t n)
{ assert(gate); return fwrite(bytes, 1, n, stdout) == n; }
void svc_store_fault(const char *point) { (void)point; }
const char *__real_svc_store_open(svc_store_t *, const char *, uint64_t, size_t (*)(void *), void *);
const char *__wrap_svc_store_open(svc_store_t *s, const char *path, uint64_t id, size_t (*free)(void *), void *context)
{
    assert(!strcmp(path, "/littlefs/svc"));
    return __real_svc_store_open(s, getenv("SVC_TEST_ROOT"), id, free, context);
}
static void add_request(const char *body, uint32_t id)
{
    size_t n = svc_frame(input + input_length, sizeof input - input_length, id, body); assert(n);
    input_length += n;
}
int main(int argc, char **argv)
{
    assert(argc == 2); case_name = argv[1];
    usb_service_config_t config = {.submit = app_maint_submit, .maintenance_begin = backend_begin,
        .maintenance_end = backend_end, .boot_ready = backend_ready, .worker_stack = backend_stack};
    assert(usb_service_init(&config) == ESP_OK);
    assert(s_boot_fingerprint_known);
    unsigned char expected[32];
    assert(svc_wifi_fingerprint(&s_engine.protocol.ports, "original", "original-password", expected));
    assert(!memcmp(expected, s_boot_fingerprint, 32));
    const esp_err_t errors[] = {ESP_OK, WIFI_MANAGER_ERR_AUTH_REJECTED, WIFI_MANAGER_ERR_NOT_RETRYING,
        WIFI_MANAGER_ERR_DRIVER_UNRESOLVED, ESP_ERR_TIMEOUT, ESP_ERR_WIFI_PASSWORD,
        ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_WIFI_NOT_INIT, ESP_ERR_WIFI_NOT_STARTED, ESP_FAIL};
    const char *reasons[] = {NULL, "wifi_auth_rejected", "wifi_not_retrying", "wifi_driver_unresolved", "wifi_timeout",
        "wifi_invalid_credentials", "wifi_invalid_credentials", "wifi_unavailable", "wifi_unavailable", "wifi_unavailable", "wifi_failed"};
    for (unsigned i = 0; i < sizeof errors / sizeof errors[0]; ++i) {
        const char *actual = wifi_reason(errors[i]);
        assert((!actual && !reasons[i]) || (actual && reasons[i] && !strcmp(actual, reasons[i])));
    }
    reject = !strcmp(case_name, "queue_failure");
    add_request("svc.bind 0123456789abcdef", 1);
    add_request("svc.wifi.join 0000000000000001 QTNCZW5jaA== d3JvbmdwYXNz", 2);
    add_request("svc.exit", 3);
    assert(usb_service_command(1, NULL) == 0);
    if (pending_worker) {
        assert(raw_submissions == 1 && usb_service_wifi_busy());
        pending_worker(worker_arg); pending_worker = NULL;
        assert(manager_calls == 1 && maintenance_ends == 1);
    } else assert(reject && manager_calls == 0 && maintenance_ends == 0);
    reject = false;
    assert(!usb_service_wifi_busy() && !maintenance);
    for (unsigned i = 0; i < 3; ++i) assert(app_maint_submit_legacy(ignored_job, NULL));
    assert(legacy_submissions == 3);
    for (unsigned i = 0; i < sizeof s_engine.password; ++i) assert(!s_engine.password[i]);
    svc_wipe(input, sizeof input); input_at = input_length = 0;
    add_request("svc.bind 0123456789abcdef", 4); add_request("svc.result 0000000000000001", 5);
    add_request("svc.stats", 6); add_request("svc.wifi.status", 7); add_request("svc.exit", 8);
    assert(usb_service_command(1, NULL) == 0);
    assert(!gate && !mutex_held && !critical);
    return 0;
}
