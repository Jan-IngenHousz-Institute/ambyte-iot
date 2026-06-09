#include <sys/time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "pcf2131tfy_rtc_api.h"
#include "RTC_NXP.h"

static const char *TAG = "pcf2131tfy_rtc_api";
static PCF2131_I2C s_pcf2131((uint8_t)(0xA6 >> 1));
static bool s_rtc_ready = false;
static esp_timer_handle_t s_sync_timer = nullptr;

extern "C" esp_err_t pcf2131tfy_rtc_init(void)
{
    s_pcf2131.begin();

    const esp_err_t err = s_pcf2131.last_error();
    s_rtc_ready = (err == ESP_OK);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC init backend failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

extern "C" bool pcf2131tfy_rtc_is_ready(void)
{
    return s_rtc_ready;
}

extern "C" esp_err_t pcf2131tfy_rtc_get_time(time_t *out_time)
{
    if (out_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_rtc_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const time_t now = s_pcf2131.time(out_time);
    const esp_err_t err = s_pcf2131.last_error();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC read failed: %s", esp_err_to_name(err));
        return err;
    }

    if (now == (time_t)(-1)) {
        ESP_LOGW(TAG, "pcf2131tfy_rtc_get_time returned invalid value");
        return ESP_FAIL;
    }

    return ESP_OK;
}

extern "C" clock_read_fn pcf2131tfy_rtc_get_clock_read_fn(void)
{
    return pcf2131tfy_rtc_get_time;
}

extern "C" esp_err_t pcf2131tfy_rtc_set_time(const struct tm *utc_tm)
{
    if (utc_tm == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_rtc_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    struct tm t = *utc_tm;   /* set() normalizes via mktime — keep caller's copy intact */
    s_pcf2131.set(&t);
    const esp_err_t err = s_pcf2131.last_error();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC set failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Push the freshly-set time into the system clock now; this also reads it
     * back, validating the write took and that the oscillator-stop flag cleared. */
    return pcf2131tfy_rtc_sync_system_clock();
}

extern "C" esp_err_t pcf2131tfy_rtc_sync_system_clock(void)
{
    time_t now = 0;
    const esp_err_t err = pcf2131tfy_rtc_get_time(&now);
    if (err != ESP_OK) {
        /* RTC unreadable or time invalid (e.g. never set / OSF) — leave the
         * system clock untouched rather than stamping data with a bogus time. */
        return err;
    }

    struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
    if (settimeofday(&tv, nullptr) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void rtc_sync_timer_cb(void *arg)
{
    (void)arg;
    const esp_err_t err = pcf2131tfy_rtc_sync_system_clock();
    if (err != ESP_OK) {
        /* Common + benign while the RTC is unset; quiet at DEBUG. */
        ESP_LOGD(TAG, "periodic RTC sync skipped: %s", esp_err_to_name(err));
    }
}

extern "C" esp_err_t pcf2131tfy_rtc_start_periodic_sync(uint32_t period_seconds)
{
    if (period_seconds == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sync_timer != nullptr) {
        return ESP_OK;  /* idempotent */
    }

    const esp_timer_create_args_t args = {
        .callback = &rtc_sync_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "rtc_sync",
        .skip_unhandled_events = false,
    };
    esp_err_t err = esp_timer_create(&args, &s_sync_timer);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_timer_start_periodic(s_sync_timer, (uint64_t)period_seconds * 1000000ULL);
    if (err != ESP_OK) {
        esp_timer_delete(s_sync_timer);
        s_sync_timer = nullptr;
        return err;
    }

    ESP_LOGI(TAG, "periodic RTC->system clock sync every %u s", (unsigned)period_seconds);
    return ESP_OK;
}
