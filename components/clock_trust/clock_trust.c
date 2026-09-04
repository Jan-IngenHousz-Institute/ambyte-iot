#include "clock_trust.h"

#include <stdint.h>
#include <sys/time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define TAG "clock_trust"

/* Own namespace: this lifetime/wear policy is independent of provisioning and
 * self-reboot latches. One write at boot/first watchdog tick and then hourly is
 * about 9k writes/year; NVS wear levelling makes that deliberately cheap. */
#define CLOCK_TRUST_NVS_NS  "time_trust"
#define CLOCK_TRUST_NVS_KEY "time_hwm"

typedef enum {
    CLOCK_SOURCE_RTC = 0,
    CLOCK_SOURCE_HWM,
    CLOCK_SOURCE_SNTP,
} clock_source_t;

static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static clock_source_t s_source = CLOCK_SOURCE_RTC;
static bool s_suspect = false;
static SemaphoreHandle_t s_nvs_mtx;

static const char *source_name(clock_source_t source)
{
    switch (source) {
        case CLOCK_SOURCE_HWM:  return "hwm";
        case CLOCK_SOURCE_SNTP: return "sntp";
        case CLOCK_SOURCE_RTC:
        default:                return "rtc";
    }
}

static void state_set(clock_source_t source, bool suspect)
{
    portENTER_CRITICAL(&s_state_mux);
    s_source = source;
    s_suspect = s_suspect || suspect; /* boot suspicion is evidence, not state */
    portEXIT_CRITICAL(&s_state_mux);
}

static esp_err_t read_hwm(uint32_t *out)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CLOCK_TRUST_NVS_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    err = nvs_get_u32(nvs, CLOCK_TRUST_NVS_KEY, out);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        return ESP_OK;
    }
    return err;
}

esp_err_t clock_trust_boot_guard(bool rtc_suspect, bool *out_adopted,
                                 time_t *out_floor)
{
    if (out_adopted != NULL) *out_adopted = false;
    if (out_floor != NULL) *out_floor = 0;
    state_set(CLOCK_SOURCE_RTC, rtc_suspect);

    /* Created before Wi-Fi/watchdog tasks exist, so all later NVS high-water
     * updates serialize. Failing to allocate only loses race protection; each
     * NVS operation still remains valid and reports its own error. */
    if (s_nvs_mtx == NULL) s_nvs_mtx = xSemaphoreCreateMutex();

    uint32_t hwm = 0;
    esp_err_t err = read_hwm(&hwm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "time high-water read failed: %s", esp_err_to_name(err));
        return err;
    }

    const time_t now = time(NULL);
    if (hwm >= (uint32_t)CLOCK_TRUST_VALID_FLOOR_S && now < (time_t)hwm) {
        struct timeval tv = { .tv_sec = (time_t)hwm, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) != 0) {
            ESP_LOGE(TAG, "failed to adopt time high-water %lu",
                     (unsigned long)hwm);
            return ESP_FAIL;
        }
        state_set(CLOCK_SOURCE_HWM, true);
        if (out_adopted != NULL) *out_adopted = true;
        if (out_floor != NULL) *out_floor = (time_t)hwm;
        ESP_LOGE(TAG, "CLOCK ROLLBACK: boot clock %lld < high-water %lu; "
                      "adopted durable floor (clock_suspect=true)",
                 (long long)now, (unsigned long)hwm);
    }
    return ESP_OK;
}

/* Shared writer for the two trust levels. Local sources (RTC/system clock —
 * `authoritative=false`) may only RAISE the stored floor: they can be wrong-fast
 * and must never lower a good floor. SNTP (`authoritative=true`) outranks the
 * floor in both directions and OVERWRITES it — bench-proven necessity
 * (2026-07-29): a clock that ran fast for one hour poisoned the monotonic
 * floor; every subsequent boot re-adopted the poisoned (future) value via the
 * rollback guard and the following SNTP correction stepped the clock BACKWARDS,
 * re-wedging every wall-clock deadline (schedule anchor, power-gate dwell) for
 * up to the poison duration. Letting network time lower the floor makes the
 * poison last one sync instead of one wall-clock catch-up. */
static esp_err_t clock_trust_write_hwm(time_t epoch_utc, bool authoritative)
{
    if (epoch_utc < (time_t)CLOCK_TRUST_VALID_FLOOR_S ||
        (uint64_t)epoch_utc > UINT32_MAX) {
        return ESP_ERR_INVALID_STATE;
    }

    bool locked = s_nvs_mtx != NULL &&
                  xSemaphoreTake(s_nvs_mtx, pdMS_TO_TICKS(5000)) == pdTRUE;
    if (s_nvs_mtx != NULL && !locked) return ESP_ERR_TIMEOUT;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CLOCK_TRUST_NVS_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        uint32_t old_hwm = 0;
        esp_err_t get_err = nvs_get_u32(nvs, CLOCK_TRUST_NVS_KEY, &old_hwm);
        if (get_err != ESP_OK && get_err != ESP_ERR_NVS_NOT_FOUND) {
            err = get_err;
        } else if ((uint32_t)epoch_utc > old_hwm ||
                   (authoritative && (uint32_t)epoch_utc != old_hwm)) {
            if (authoritative && (uint32_t)epoch_utc < old_hwm) {
                ESP_LOGW(TAG, "SNTP lowered poisoned time high-water %u -> %u "
                              "(floor was ahead of true time)",
                         (unsigned)old_hwm, (unsigned)epoch_utc);
            }
            err = nvs_set_u32(nvs, CLOCK_TRUST_NVS_KEY, (uint32_t)epoch_utc);
            if (err == ESP_OK) err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (locked) xSemaphoreGive(s_nvs_mtx);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "time high-water refresh failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t clock_trust_set_hwm_authoritative(time_t epoch_utc)
{
    return clock_trust_write_hwm(epoch_utc, true);
}

esp_err_t clock_trust_refresh_hwm_at(time_t epoch_utc)
{
    return clock_trust_write_hwm(epoch_utc, false);
}

esp_err_t clock_trust_refresh_hwm(void)
{
    return clock_trust_refresh_hwm_at(time(NULL));
}

void clock_trust_note_sntp(void)
{
    state_set(CLOCK_SOURCE_SNTP, false);
}

void clock_trust_note_rtc(void)
{
    state_set(CLOCK_SOURCE_RTC, false);
}

void clock_trust_get_status(const char **out_source, bool *out_suspect)
{
    portENTER_CRITICAL(&s_state_mux);
    clock_source_t source = s_source;
    bool suspect = s_suspect;
    portEXIT_CRITICAL(&s_state_mux);
    if (out_source != NULL) *out_source = source_name(source);
    if (out_suspect != NULL) *out_suspect = suspect;
}
