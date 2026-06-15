#include "device_commands.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "ambit_protocol.h"

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wifi_manager.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "dev_cmd"

#define MQTT_TOPIC_MAX        256
#define MQTT_PAYLOAD_MAX     (128U * 1024U)   /* AWS IoT max payload */

static device_commands_config_t s_cfg;
static bool s_initialized = false;
static char s_mac_str[18]; /* "XX:XX:XX:XX:XX:XX\0" */

/* In-flight publish tracking. The sync runner claims a batch of measurement
 * groups, the MQTT layer publishes them as one message, and the broker's
 * PUBACK matches by msg_id back to the listed measure_ids. The mutex
 * serialises read/mutate access across the Lua task, sync runner, and the
 * MQTT event task that fires on_publish_ack. */
static SemaphoreHandle_t s_inflight_mtx = NULL;
static StaticSemaphore_t s_inflight_mtx_storage;
/* One event in flight at a time (one measure_id = one MQTT message). */
static int64_t s_inflight_measure_id = -1;
static int     s_inflight_msg_id     = -1;

/* Measurement-activity gate. Incremented around every UART sensor transaction
 * so the background sync runner can pause publishing while a measurement is in
 * progress (it drains during idle/sleep instead). A counter (not a bool) so
 * overlapping reads don't clear it early. Single 32-bit access is atomic on
 * the ESP32; the sync runner only reads it. */
static volatile int s_measurement_active = 0;

static cmd_result_t make_result(esp_err_t status, const char *fmt, ...)
{
    cmd_result_t r;
    r.status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.message, sizeof(r.message), fmt, ap);
    va_end(ap);
    return r;
}

/* ── Sync-runner wake hook ───────────────────────────────────────────────
 * The sole publisher (sync_runner) registers a task-notify here so it can sleep
 * until there's work instead of polling. Fired after every stored event and when
 * a measurement burst finishes (so a drain deferred during the burst resumes). */
static void (*s_sync_notifier)(void) = NULL;

void device_commands_set_sync_notifier(void (*fn)(void)) { s_sync_notifier = fn; }

static inline void notify_sync(void)
{
    void (*fn)(void) = s_sync_notifier;   /* single read; set once at boot */
    if (fn != NULL) fn();
}

/* Internal ack handler — called by mqtt_client on MQTT_EVENT_PUBLISHED */
static void on_publish_ack(int msg_id, esp_err_t status, void *ctx)
{
    (void)ctx;
    if (s_inflight_mtx == NULL) return;
    if (xSemaphoreTake(s_inflight_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return;

    if (s_inflight_msg_id < 0 || msg_id != s_inflight_msg_id) {
        xSemaphoreGive(s_inflight_mtx);
        return; /* Unknown ack — ignore (e.g. raw cmd_mqtt_publish) */
    }
    int64_t mid = s_inflight_measure_id;
    s_inflight_measure_id = -1;
    s_inflight_msg_id     = -1;
    xSemaphoreGive(s_inflight_mtx);

    if (status == ESP_OK && s_cfg.mark_event_synced != NULL) {
        s_cfg.mark_event_synced(mid);
    } else if (s_cfg.mark_event_pending != NULL) {
        s_cfg.mark_event_pending(mid);
    }
}

esp_err_t device_commands_init(const device_commands_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    s_initialized = true;

    if (s_inflight_mtx == NULL) {
        s_inflight_mtx = xSemaphoreCreateMutexStatic(&s_inflight_mtx_storage);
    }

    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        s_mac_str[0] = '\0';
    }

    if (s_cfg.set_publish_ack_handler != NULL) {
        s_cfg.set_publish_ack_handler(on_publish_ack, NULL);
    }

    ESP_LOGI(TAG, "Device commands initialized");
    ESP_LOGI(TAG, "  MAC:            %s", s_mac_str[0] ? s_mac_str : "(unavail)");
    ESP_LOGI(TAG, "  topic_root:     %s", s_cfg.topic_root      ? s_cfg.topic_root      : "(null)");
    ESP_LOGI(TAG, "  device_id:      %s", s_cfg.device_id       ? s_cfg.device_id       : "(null)");
    ESP_LOGI(TAG, "  protocol_id:    %s", s_cfg.protocol_id     ? s_cfg.protocol_id     : "(null)");
    ESP_LOGI(TAG, "  device_name:    %s", s_cfg.device_name     ? s_cfg.device_name     : "(null)");
    ESP_LOGI(TAG, "  device_version: %s", s_cfg.device_version  ? s_cfg.device_version  : "(null)");
    ESP_LOGI(TAG, "  device_firm:    %s", s_cfg.device_firmware ? s_cfg.device_firmware : "(null)");
    ESP_LOGI(TAG, "  timezone:       %s", (s_cfg.timezone && s_cfg.timezone[0]) ? s_cfg.timezone : "(unset)");
    return ESP_OK;
}

const char *device_commands_get_mac(void)
{
    return s_mac_str;   /* "" if esp_wifi_get_mac failed at init */
}

cmd_result_t cmd_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized || s_cfg.set_status == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "status LED not available");
    }
    esp_err_t err = s_cfg.set_status(r, g, b);
    if (err != ESP_OK) {
        return make_result(err, "set_rgb failed: %s", esp_err_to_name(err));
    }
    return make_result(ESP_OK, "RGB set to (%u, %u, %u)", r, g, b);
}

/* ── PWM output (LEDC on GPIO4) ─────────────────────────────────────────
 *
 * A single LEDC timer/channel drives GPIO4. The duty resolution is chosen per
 * call as the largest that supports the requested frequency on the 80 MHz APB
 * clock (ESP32-S3 LEDC tops out at 14-bit). */
#define PWM_GPIO          4
#define PWM_TIMER         LEDC_TIMER_0
#define PWM_CHANNEL       LEDC_CHANNEL_0
#define PWM_SPEED_MODE    LEDC_LOW_SPEED_MODE   /* S3 has no high-speed mode */
#define PWM_SRC_CLK_HZ    80000000u             /* APB clock */
#define PWM_MAX_RES_BITS  14                    /* SOC_LEDC_TIMER_BIT_WIDTH (S3) */

static bool s_pwm_configured = false;

/* Largest duty resolution (bits) whose full-scale period fits freq_hz on the
 * APB clock. Returns 0 if the frequency is too high to represent. */
static int pwm_pick_resolution(uint32_t freq_hz)
{
    int bits = 0;
    while (bits < PWM_MAX_RES_BITS &&
           (1u << (bits + 1)) <= (PWM_SRC_CLK_HZ / freq_hz)) {
        bits++;
    }
    return bits;
}

cmd_result_t cmd_pwm(float duty_pct, uint32_t freq_hz, bool enable)
{
    if (duty_pct < 0.0f || duty_pct > 100.0f) {
        return make_result(ESP_ERR_INVALID_ARG,
                           "duty must be 0..100 (got %.2f)", (double)duty_pct);
    }

    if (!enable) {
        if (s_pwm_configured) {
            ledc_stop(PWM_SPEED_MODE, PWM_CHANNEL, 0);  /* hold pin low */
        }
        return make_result(ESP_OK, "PWM disabled on GPIO%d", PWM_GPIO);
    }

    if (freq_hz == 0) {
        return make_result(ESP_ERR_INVALID_ARG, "freq must be > 0");
    }
    int bits = pwm_pick_resolution(freq_hz);
    if (bits == 0) {
        return make_result(ESP_ERR_INVALID_ARG, "freq %u Hz too high (max %u Hz)",
                           (unsigned)freq_hz, (unsigned)(PWM_SRC_CLK_HZ / 2));
    }

    ledc_timer_config_t tcfg = {
        .speed_mode      = PWM_SPEED_MODE,
        .timer_num       = PWM_TIMER,
        .duty_resolution = (ledc_timer_bit_t)bits,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_USE_APB_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        return make_result(err, "ledc_timer_config(%u Hz, %d-bit) failed: %s",
                           (unsigned)freq_hz, bits, esp_err_to_name(err));
    }

    uint32_t full = 1u << bits;
    uint32_t raw  = (uint32_t)lroundf(duty_pct / 100.0f * (float)full);
    if (raw > full) {
        raw = full;
    }

    /* Re-running channel_config each enable is idempotent and re-asserts the
     * output after a prior disable (ledc_stop). */
    ledc_channel_config_t ccfg = {
        .gpio_num   = PWM_GPIO,
        .speed_mode = PWM_SPEED_MODE,
        .channel    = PWM_CHANNEL,
        .timer_sel  = PWM_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .duty       = raw,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        return make_result(err, "ledc_channel_config failed: %s",
                           esp_err_to_name(err));
    }
    s_pwm_configured = true;

    return make_result(ESP_OK, "PWM GPIO%d: %.2f%% @ %u Hz (%d-bit)",
                       PWM_GPIO, (double)duty_pct, (unsigned)freq_hz, bits);
}

cmd_result_t cmd_read_rtc(time_t *out_time)
{
    if (!s_initialized || s_cfg.read_clock == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "RTC not available");
    }
    if (out_time == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "out_time is NULL");
    }
    esp_err_t err = s_cfg.read_clock(out_time);
    if (err != ESP_OK) {
        return make_result(err, "RTC read failed: %s", esp_err_to_name(err));
    }
    return make_result(ESP_OK, "RTC: %lld", (long long)*out_time);
}

cmd_result_t cmd_device_status(bool *bme_ready, bool *rtc_ready, time_t *rtc_time)
{
    bool bme_ok = false;
    bool rtc_ok = false;

    if (s_initialized && s_cfg.read_env != NULL) {
        measurement_t m;
        bme_ok = (s_cfg.read_env(&m) == ESP_OK);
    }

    if (s_initialized && s_cfg.read_clock != NULL) {
        time_t t = 0;
        if (s_cfg.read_clock(&t) == ESP_OK) {
            rtc_ok = true;
            if (rtc_time != NULL) {
                *rtc_time = t;
            }
        }
    }

    if (bme_ready != NULL) {
        *bme_ready = bme_ok;
    }
    if (rtc_ready != NULL) {
        *rtc_ready = rtc_ok;
    }

    return make_result(ESP_OK, "BME280=%s RTC=%s",
                       bme_ok ? "ok" : "unavail",
                       rtc_ok ? "ok" : "unavail");
}

cmd_result_t cmd_read_env(float *temp, float *hum, float *pres)
{
    if (!s_initialized || s_cfg.read_env == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "env sensor not available");
    }
    measurement_t m;
    esp_err_t err = s_cfg.read_env(&m);
    if (err != ESP_OK) {
        return make_result(err, "env read failed: %s", esp_err_to_name(err));
    }
    if (temp != NULL) *temp = m.temperature_c;
    if (hum != NULL)  *hum = m.humidity_percent;
    if (pres != NULL) *pres = m.pressure_pa;
    return make_result(ESP_OK, "T=%.2fC H=%.1f%% P=%.0fPa",
                       m.temperature_c, m.humidity_percent, m.pressure_pa);
}

cmd_result_t cmd_read_power(power_reading_t *out)
{
    if (!s_initialized || s_cfg.read_power == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "power monitor not available");
    }
    power_reading_t p;
    esp_err_t err = s_cfg.read_power(&p);
    if (err != ESP_OK) {
        return make_result(err, "power read failed: %s", esp_err_to_name(err));
    }
    if (out != NULL) *out = p;
    return make_result(ESP_OK, "Vbat=%umV Vin=%umV Vsys=%umV Iin=%umA Icc=%umA",
                       p.battery_mv, p.input_mv, p.system_mv, p.input_ma, p.charge_ma);
}

/* Return current UTC milliseconds since epoch, sourced from the IDF's internal
 * clock (settimeofday'd from the RTC at boot — see components/pcf2131tfy_rtc).
 * Cheap: one syscall, no I2C transactions. */
static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

/* ── Publish power gate (Phase 1) ─────────────────────────────────────────
 * Only drain the MQTT backlog while on external power. Keyed on VIN-present
 * (input voltage / charger VIN status) rather than input current: Iin has a
 * ~13 mA ADC step and reads near-zero when the battery is full even in full
 * sun, so it under-reports available power. The charger reading is cached
 * (PUB_GATE_EVAL_INTERVAL_MS) so the drain loop's frequent re-checks stay cheap,
 * and the gate is debounced with asymmetric dwell times so a reading hovering
 * at the threshold can't toggle publishing on/off rapidly. */
#define PUB_GATE_EVAL_INTERVAL_MS   5000    /* re-read the charger at most this often */
#define PUB_GATE_ON_DWELL_MS       15000    /* power present this long ⇒ open the gate  */
#define PUB_GATE_OFF_DWELL_MS      60000    /* power absent this long  ⇒ close the gate */

static bool    s_pub_gate_open    = false;  /* debounced gate state (closed at boot) */
static bool    s_pub_gate_cand    = false;  /* last instantaneous reading vs. gate */
static int64_t s_pub_gate_cand_ms = 0;      /* when cand first differed from the gate */
static int64_t s_pub_gate_eval_ms = 0;      /* last charger evaluation (0 = never) */

/* Last known battery voltage, latched on every successful charger read (the
 * power gate and status report both feed it). 0 = never read; the envelope's
 * `device_battery` field is omitted in that case. */
static uint32_t s_last_batt_mv = 0;

bool device_commands_publish_power_ok(void)
{
    /* No power monitor wired in (dev board / absent charger): never gate, so
     * publishing behaves exactly as before this feature existed. */
    if (!s_initialized || s_cfg.read_power == NULL) {
        return true;
    }

    const int64_t now = now_ms();
    if (s_pub_gate_eval_ms != 0 &&
        (now - s_pub_gate_eval_ms) < PUB_GATE_EVAL_INTERVAL_MS) {
        return s_pub_gate_open;   /* cached between evaluations */
    }
    s_pub_gate_eval_ms = now;

    power_reading_t p;
    if (s_cfg.read_power(&p) != ESP_OK) {
        /* Transient I2C/ADC failure: hold the last decision rather than flap. */
        return s_pub_gate_open;
    }
    s_last_batt_mv = p.battery_mv;
    const bool present = p.input_present;

    if (present == s_pub_gate_open) {
        s_pub_gate_cand = present;            /* steady — clear any pending change */
    } else if (present != s_pub_gate_cand) {
        s_pub_gate_cand    = present;         /* new candidate — start the dwell timer */
        s_pub_gate_cand_ms = now;
    } else {
        const int64_t dwell = present ? PUB_GATE_ON_DWELL_MS : PUB_GATE_OFF_DWELL_MS;
        if ((now - s_pub_gate_cand_ms) >= dwell) {
            s_pub_gate_open = present;        /* debounce satisfied — flip the gate */
            ESP_LOGI(TAG, "publish gate %s (Vin=%umV Ibat=%umA)",
                     present ? "OPEN (external power)" : "CLOSED (on battery)",
                     p.input_mv, p.charge_ma);
        }
    }
    return s_pub_gate_open;
}

cmd_result_t cmd_record_env(int64_t *out_measure_id, measurement_t *out_reading)
{
    if (!s_initialized || s_cfg.read_env == NULL ||
        s_cfg.store_event == NULL || s_cfg.next_id == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "env/persistence not available");
    }

    int64_t start_ms = now_ms();
    measurement_t m;
    esp_err_t err = s_cfg.read_env(&m);
    int64_t end_ms = now_ms();
    if (err != ESP_OK) {
        return make_result(err, "env read failed: %s", esp_err_to_name(err));
    }
    if (out_reading) *out_reading = m;

    int64_t mid = 0;
    if ((err = s_cfg.next_id(&mid)) != ESP_OK) {
        return make_result(err, "next_id failed: %s", esp_err_to_name(err));
    }

    /* One event: T/H/P together in the payload. channel "" = onboard sensor. */
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.1f}",
             m.temperature_c, m.humidity_percent, m.pressure_pa);

    measurement_event_desc_t d = {
        .measure_id   = mid,
        .tag          = MEASUREMENT_TAG_MEASUREMENT,
        .cmd_raw      = "device.bme280",
        .start_ms     = start_ms,
        .end_ms       = end_ms,
        .payload_json = payload,
    };
    err = s_cfg.store_event(&d);
    if (err != ESP_OK) {
        return make_result(err, "store failed: %s", esp_err_to_name(err));
    }
    notify_sync();   /* wake the publisher */

    if (out_measure_id) *out_measure_id = mid;
    return make_result(ESP_OK,
                       "recorded env id=%lld: T=%.2fC H=%.1f%% P=%.0fPa",
                       (long long)mid, m.temperature_c,
                       m.humidity_percent, m.pressure_pa);
}

cmd_result_t cmd_log(const char *msg)
{
    if (msg == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "msg is NULL");
    }
    ESP_LOGI(TAG, "%s", msg);
    return make_result(ESP_OK, "logged");
}

cmd_result_t cmd_sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
    return make_result(ESP_OK, "slept %lu ms", (unsigned long)ms);
}

cmd_result_t cmd_sd_ready(bool *out_ready)
{
    if (out_ready == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "out_ready is NULL");
    }
    if (!s_initialized || s_cfg.sd_ready == NULL) {
        *out_ready = false;
        return make_result(ESP_ERR_NOT_SUPPORTED, "SD port not wired");
    }
    *out_ready = s_cfg.sd_ready();
    return make_result(ESP_OK, "SD: %s", *out_ready ? "ready" : "out");
}

cmd_result_t cmd_next_measure_id(int64_t *out_id)
{
    if (!s_initialized || s_cfg.next_id == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "persistence not available");
    }
    esp_err_t err = s_cfg.next_id(out_id);
    if (err != ESP_OK) {
        return make_result(err, "next_id failed: %s", esp_err_to_name(err));
    }
    return make_result(ESP_OK, "next_id: %lld", (long long)*out_id);
}

cmd_result_t cmd_mqtt_status(void)
{
    if (!s_initialized || s_cfg.message_is_connected == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "MQTT not available");
    }
    bool connected = s_cfg.message_is_connected();
    return make_result(ESP_OK, "MQTT: %s", connected ? "connected" : "disconnected");
}

cmd_result_t cmd_db_status(bool *available, int64_t *total,
                           int64_t *pending, int64_t *next_id)
{
    if (!s_initialized || s_cfg.db_stats == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "persistence not available");
    }
    bool    avail = false;
    int64_t tot = 0, pend = 0, nid = 0;
    esp_err_t err = s_cfg.db_stats(&avail, &tot, &pend, &nid);
    if (err != ESP_OK) {
        return make_result(err, "db stats failed: %s", esp_err_to_name(err));
    }
    if (available) *available = avail;
    if (total)     *total     = tot;
    if (pending)   *pending   = pend;
    if (next_id)   *next_id   = nid;
    return make_result(ESP_OK, "DB=%s total=%lld pending=%lld next_id=%lld",
                       avail ? "online" : "offline",
                       (long long)tot, (long long)pend, (long long)nid);
}

/* ── Event store + publish (one row/event; one message per measure_id) ── */

cmd_result_t cmd_store_event(const measurement_event_desc_t *desc)
{
    if (!s_initialized || s_cfg.store_event == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "event storage not available (no SD?)");
    }
    if (desc == NULL || desc->payload_json == NULL ||
        desc->tag == NULL || desc->tag[0] == '\0') {
        return make_result(ESP_ERR_INVALID_ARG, "store_event: null arg");
    }
    esp_err_t err = s_cfg.store_event(desc);
    if (err != ESP_OK) {
        return make_result(err, "store_event(%s) failed: %s",
                           desc->cmd_raw ? desc->cmd_raw : "", esp_err_to_name(err));
    }
    notify_sync();   /* wake the publisher */
    return make_result(ESP_OK, "stored event id=%lld (%s)",
                       (long long)desc->measure_id, desc->cmd_raw ? desc->cmd_raw : "");
}

/* Publish the next pending event as one MQTT message (one measure_id = one
 * message). Keeps the cloud's `sample:[…]` wrapper; the event's quantities are
 * nested under `data`. */
cmd_result_t cmd_mqtt_publish_next_event(void)
{
    if (!s_initialized || s_cfg.publish == NULL ||
        s_cfg.claim_next_event == NULL || s_cfg.mark_event_pending == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "MQTT or persistence not available");
    }
    /* Don't even claim an event if the broker isn't connected — Wi-Fi/MQTT
     * may be down and we'd just shove bytes at a dead pipe (the esp-mqtt
     * client silently enqueues / drops them). Keeps events PENDING for the
     * next cycle and lets sync_runner_drain exit silently. */
    if (s_cfg.message_is_connected != NULL && !s_cfg.message_is_connected()) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "MQTT not connected");
    }
    /* Power gate lives solely in sync_runner_is_allowed() now — sync_runner is
     * the only caller of this function (Lua can no longer publish directly), so
     * one check there is sufficient and unbypassable. */
    if (s_inflight_mtx == NULL) {
        return make_result(ESP_ERR_INVALID_STATE, "inflight mutex not initialised");
    }

    /* One event in flight at a time. */
    if (xSemaphoreTake(s_inflight_mtx, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return make_result(ESP_ERR_TIMEOUT, "inflight mutex busy");
    }
    bool busy = (s_inflight_msg_id >= 0);
    xSemaphoreGive(s_inflight_mtx);
    if (busy) {
        return make_result(ESP_ERR_INVALID_STATE, "previous event still in flight");
    }

    measurement_event_t e;
    esp_err_t err = s_cfg.claim_next_event(&e);
    if (err == ESP_ERR_NOT_FOUND) {
        return make_result(ESP_ERR_NOT_FOUND, "no pending measurements");
    }
    if (err != ESP_OK) {
        return make_result(err, "claim_next_event failed: %s", esp_err_to_name(err));
    }

    /* Build the MQTT envelope (schema v2) as a string, splicing the already-valid
     * payload (and metadata) JSON in verbatim — no cJSON_Parse round-trip. The old
     * parse→tree→print path needed ~4× the payload in heap (a node per number),
     * which OOMs on this tight heap for multi-array runs, and scaled with point
     * count. This needs one buffer ≈ payload size + a fixed envelope. The
     * channel/device/tag/cmd_raw/config strings are controlled (firmware-built or
     * provisioned, sanitized by the event log) so they are quoted directly;
     * payload_json/metadata_json are already valid JSON.
     *
     * Envelope `timestamp` = the MEASUREMENT time (startTicks): the cloud
     * pipeline aliases it to measurement_time_utc, so battery-queued events must
     * carry their capture time, not the publish time. Publish time goes into the
     * sample as `published`. */
    char meas_ts[32], pub_ts[32];
    {
        struct tm tm_info;
        time_t ts = (time_t)(e.start_ticks_ms / 1000);
        gmtime_r(&ts, &tm_info);
        strftime(meas_ts, sizeof(meas_ts), "%Y-%m-%dT%H:%M:%SZ", &tm_info);
        ts = time(NULL);
        gmtime_r(&ts, &tm_info);
        strftime(pub_ts, sizeof(pub_ts), "%Y-%m-%dT%H:%M:%SZ", &tm_info);
    }

    /* "" → JSON null for the optional provenance strings. */
    char chanbuf[16], devbuf[32];
    if (e.channel[0] != '\0') snprintf(chanbuf, sizeof(chanbuf), "\"%s\"", e.channel);
    else                      strcpy(chanbuf, "null");
    if (e.device[0] != '\0')  snprintf(devbuf, sizeof(devbuf), "\"%s\"", e.device);
    else                      strcpy(devbuf, "null");
    /* cmd_raw is variable-length (a full "arrun …" can be ~520 B) → heap-quoted;
     * NULL falls back to the JSON null literal in the splice below. */
    char *cmdbuf = NULL;
    if (e.cmd_raw != NULL && e.cmd_raw[0] != '\0') {
        size_t cn = strlen(e.cmd_raw);
        cmdbuf = malloc(cn + 3);
        if (cmdbuf == NULL) {
            s_cfg.mark_event_pending(e.measure_id);
            measurement_event_free(&e);
            return make_result(ESP_ERR_NO_MEM, "cmd_raw buf alloc failed (%u B)", (unsigned)(cn + 3));
        }
        cmdbuf[0] = '"';
        memcpy(cmdbuf + 1, e.cmd_raw, cn);
        cmdbuf[cn + 1] = '"';
        cmdbuf[cn + 2] = '\0';
    }
    const char *cmdfield = cmdbuf ? cmdbuf : "null";

    /* Optional envelope fields — empty string when absent. */
    char battpart[48] = "";
    if (s_last_batt_mv != 0) {
        snprintf(battpart, sizeof(battpart), "\"device_battery\":%.3f,",
                 (double)s_last_batt_mv / 1000.0);
    }
    char tzpart[96] = "";
    if (s_cfg.timezone != NULL && s_cfg.timezone[0] != '\0') {
        snprintf(tzpart, sizeof(tzpart), "\"timezone\":\"%s\",", s_cfg.timezone);
    }

    const char *meta = e.metadata_json;          /* already a JSON object, or NULL */
    const char *fw   = s_cfg.device_firmware  ? s_cfg.device_firmware  : "";
    const char *dn   = s_cfg.device_name      ? s_cfg.device_name      : "";
    const char *dv   = s_cfg.device_version   ? s_cfg.device_version   : "";

    size_t cap = strlen(e.payload_json) + (meta ? strlen(meta) : 4)
               + strlen(chanbuf) + strlen(devbuf) + strlen(cmdfield) + strlen(e.tag)
               + strlen(battpart) + strlen(tzpart)
               + strlen(fw) + strlen(dn) + strlen(dv)
               + strlen(s_mac_str) + sizeof(meas_ts) + sizeof(pub_ts)
               + 320;                            /* fixed keys + numbers + punctuation */
    char *payload = malloc(cap);
    if (payload == NULL) {
        free(cmdbuf);
        s_cfg.mark_event_pending(e.measure_id);
        measurement_event_free(&e);
        return make_result(ESP_ERR_NO_MEM, "envelope alloc failed (%u B)", (unsigned)cap);
    }

    int n = snprintf(payload, cap,
        "{\"sample\":[{"
            "\"v\":2,\"measure_id\":%lld,\"startTicks\":%lld,\"endTicks\":%lld,"
            "\"published\":\"%s\",\"channel\":%s,\"device\":%s,"
            "\"cmd_raw\":%s,\"tag\":\"%s\",\"metadata\":%s,\"data\":%s"
        "}],"
        "\"timestamp\":\"%s\",%s%s"
        "\"device_id\":\"%s\",\"device_name\":\"%s\","
        "\"device_version\":\"%s\",\"device_firmware\":\"%s\"}",
        (long long)e.measure_id, (long long)e.start_ticks_ms, (long long)e.end_ticks_ms,
        pub_ts, chanbuf, devbuf, cmdfield, e.tag,
        meta ? meta : "null", e.payload_json,
        meas_ts, battpart, tzpart,
        s_mac_str, dn, dv, fw);
    free(cmdbuf);

    if (n < 0 || (size_t)n >= cap) {
        free(payload);
        s_cfg.mark_event_pending(e.measure_id);
        measurement_event_free(&e);
        return make_result(ESP_ERR_NO_MEM, "envelope build failed (n=%d cap=%u)",
                           n, (unsigned)cap);
    }

    size_t payload_len = (size_t)n;
    if (payload_len >= MQTT_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "event payload %u bytes exceeds %u; may be rejected",
                 (unsigned)payload_len, (unsigned)MQTT_PAYLOAD_MAX);
    }
    char topic[MQTT_TOPIC_MAX];
    snprintf(topic, sizeof(topic), "%s/1234", s_cfg.topic_root ? s_cfg.topic_root : "");

    ESP_LOGI(TAG, "publish event -> %s (id=%lld, tag=%s, ch=%s, %u bytes)",
             topic, (long long)e.measure_id, e.tag,
             e.channel[0] ? e.channel : "-", (unsigned)payload_len);

    int msg_id = 0;
    err = s_cfg.publish(topic, payload, payload_len, &msg_id);
    free(payload);
    if (err != ESP_OK) {
        s_cfg.mark_event_pending(e.measure_id);
        measurement_event_free(&e);
        return make_result(err, "event publish failed: %s", esp_err_to_name(err));
    }

    if (xSemaphoreTake(s_inflight_mtx, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_inflight_measure_id = e.measure_id;
        s_inflight_msg_id     = msg_id;
        xSemaphoreGive(s_inflight_mtx);
    } else {
        ESP_LOGW(TAG, "inflight latch failed; event may be re-published on retry");
    }

    int64_t mid = e.measure_id;
    measurement_event_free(&e);
    return make_result(ESP_OK, "published event id=%lld msg_id=%d", (long long)mid, msg_id);
}

/* ── Status report + heartbeat publish ─────────────────────────────────── */

cmd_result_t cmd_status_report(device_status_snapshot_t *out)
{
    if (out == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "status_report: null out");
    }
    memset(out, 0, sizeof(*out));

    out->wifi_connected = wifi_manager_is_connected();
    (void)wifi_manager_is_provisioned(&out->provisioned);

    int64_t total = 0, pending = 0, next_id = 0;
    (void)cmd_db_status(&out->db_online, &total, &pending, &next_id);

    if (s_cfg.read_power != NULL && s_cfg.read_power(&out->power) == ESP_OK) {
        out->power_valid = true;
        s_last_batt_mv = out->power.battery_mv;
    }
    out->publish_gate_open = device_commands_publish_power_ok();

    /* On-board BME280 environment, so every heartbeat carries T/H/P even when
     * main.lua isn't measuring (broken/missing script, or power-gated). Same
     * keys/format as the device.bme280 measurement event (cmd_record_env). */
    if (s_cfg.read_env != NULL) {
        measurement_t m;
        if (s_cfg.read_env(&m) == ESP_OK) {
            out->env_valid         = true;
            out->temperature_c     = m.temperature_c;
            out->humidity_percent  = m.humidity_percent;
            out->pressure_pa       = m.pressure_pa;
        }
    }

    return make_result(ESP_OK, "status report");
}

/* Build + store one STATUS heartbeat event from the live status snapshot
 * (tag STATUS, onboard provenance — channel/cmd_raw null). Owned by the
 * sync_runner heartbeat (payload-v2 Phase 4): status reporting must survive a
 * missing/crashed main.lua, so it does NOT live in the script. Payload keys
 * match the old Lua status_report table for analysis continuity; power fields
 * are omitted when the charger read fails, and BME280 T/H/P fields are appended
 * when the env read succeeds. Does NOT notify the sync runner — the caller IS
 * the sync runner. */
cmd_result_t cmd_store_status_event(void)
{
    if (!s_initialized || s_cfg.store_event == NULL || s_cfg.next_id == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "persistence not available");
    }

    device_status_snapshot_t s;
    cmd_result_t r = cmd_status_report(&s);
    if (r.status != ESP_OK) {
        return r;
    }

    /* Base status, then power and env each append their block when the read
     * succeeded — so the payload reflects exactly which subsystems were live
     * this cycle. Env keys/format match the device.bme280 measurement event. */
    char payload[384];
    int n = snprintf(payload, sizeof(payload),
        "{\"wifi\":%s,\"provisioned\":%s,\"db_online\":%s,\"publish_gate\":%s",
        s.wifi_connected ? "true" : "false",
        s.provisioned ? "true" : "false",
        s.db_online ? "true" : "false",
        s.publish_gate_open ? "true" : "false");
    if (n > 0 && n < (int)sizeof(payload) && s.power_valid) {
        n += snprintf(payload + n, sizeof(payload) - n,
            ",\"battery_v\":%.3f,\"input_v\":%.3f,\"system_v\":%.3f,"
            "\"input_ma\":%u,\"charge_ma\":%u,\"input_present\":%s,\"charge_status\":%u",
            (double)s.power.battery_mv / 1000.0,
            (double)s.power.input_mv / 1000.0,
            (double)s.power.system_mv / 1000.0,
            (unsigned)s.power.input_ma,
            (unsigned)s.power.charge_ma,
            s.power.input_present ? "true" : "false",
            (unsigned)s.power.charge_status);
    }
    if (n > 0 && n < (int)sizeof(payload) && s.env_valid) {
        n += snprintf(payload + n, sizeof(payload) - n,
            ",\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.1f",
            s.temperature_c, s.humidity_percent, s.pressure_pa);
    }
    if (n > 0 && n < (int)sizeof(payload)) {
        n += snprintf(payload + n, sizeof(payload) - n, "}");
    }
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        return make_result(ESP_FAIL, "status payload build failed");
    }

    int64_t mid = 0;
    esp_err_t err = s_cfg.next_id(&mid);
    if (err != ESP_OK) {
        return make_result(err, "next_id failed: %s", esp_err_to_name(err));
    }

    int64_t now = now_ms();
    measurement_event_desc_t d = {
        .measure_id   = mid,
        .tag          = MEASUREMENT_TAG_STATUS,
        .start_ms     = now,
        .end_ms       = now,
        .payload_json = payload,
    };
    err = s_cfg.store_event(&d);
    if (err != ESP_OK) {
        return make_result(err, "status store failed: %s", esp_err_to_name(err));
    }
    return make_result(ESP_OK, "STATUS id=%lld gate=%s Vbat=%umV",
                       (long long)mid, s.publish_gate_open ? "OPEN" : "CLOSED",
                       (unsigned)(s.power_valid ? s.power.battery_mv : 0));
}

/* Last battery voltage latched from any successful charger read (power gate /
 * status report). 0 = never read. Probe for the status-LED blinker. */
uint32_t device_commands_last_battery_mv(void)
{
    return s_last_batt_mv;
}

cmd_result_t cmd_uart_stream_query(uint8_t channel, const char *cmd,
                                   const char *sentinel, uint32_t timeout_ms,
                                   char *out, size_t out_cap, size_t *out_len)
{
    if (!s_initialized || s_cfg.uart_stream_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART stream query not available");
    }
    if (cmd == NULL || out == NULL || out_len == NULL || out_cap < 2) {
        return make_result(ESP_ERR_INVALID_ARG, "stream_query: bad args");
    }
    device_commands_measurement_begin();
    esp_err_t err = s_cfg.uart_stream_query(channel, cmd, "\n", sentinel,
                                            out, out_cap, out_len, timeout_ms);
    device_commands_measurement_end();
    if (err == ESP_ERR_TIMEOUT) {
        return make_result(ESP_ERR_TIMEOUT, "stream timeout (no '%s')", sentinel ? sentinel : "");
    }
    if (err != ESP_OK) {
        return make_result(err, "stream_query ch%u failed: %s", channel, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "stream ch%u: %u bytes", channel, (unsigned)*out_len);
}

void device_commands_on_mqtt_disconnect(void)
{
    if (s_inflight_mtx == NULL) return;
    if (xSemaphoreTake(s_inflight_mtx, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    int64_t mid = s_inflight_measure_id;
    bool had = (mid >= 0 && s_inflight_msg_id >= 0);
    s_inflight_measure_id = -1;
    s_inflight_msg_id     = -1;
    xSemaphoreGive(s_inflight_mtx);

    if (had && s_cfg.mark_event_pending != NULL) {
        s_cfg.mark_event_pending(mid);
    }
}

/* ── Measurement-activity gate ──────────────────────────────────────────
 * The sync runner consults device_commands_measurement_active() and pauses
 * publishing while a UART measurement is in progress, draining during idle. */
void device_commands_measurement_begin(void) { s_measurement_active++; }
void device_commands_measurement_end(void)
{
    if (s_measurement_active > 0) s_measurement_active--;
    if (s_measurement_active == 0) notify_sync();  /* burst done — let the runner drain */
}
bool device_commands_measurement_active(void) { return s_measurement_active > 0; }

/* ── UART sensor commands — raw interface (Phase 7) ─────────────── */

/* Generic binary query: sends the 8-byte `cmd` (+ optional `extra` payload)
 * to an AMBIT sensor on `channel` (0-3) using the ambit-1 ESP protocol.
 *
 * `expect_raw` selects the response mode:
 *   UART_QUERY_ACK_ONLY  — return after CMD_DONE (0xA1); no response data, no CMD_END
 *   0                    — FSM data-transfer mode: wait for structured array data via
 *                          handshake (for measurement commands 20, 21)
 *   >0                   — immediate raw response: read exactly N bytes after CMD_DONE,
 *                          then verify CMD_END (0xF0) follows (for query commands 31-34)
 *
 * Caller must free response via uart_sensor_response_free().
 * Lua: device.uart_query(ch, {cmd_bytes}, extra_or_nil, expect_raw, timeout_ms)
 * CLI: not exposed directly (use typed ambit_* commands instead)                      */
cmd_result_t cmd_uart_query(uint8_t channel, const uint8_t cmd[8],
                            const uint8_t *extra, size_t extra_len,
                            size_t expect_raw,
                            uart_sensor_response_t *response,
                            uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (channel >= UART_SENSOR_NUM_CHANNELS) {
        return make_result(ESP_ERR_INVALID_ARG, "invalid channel %u", channel);
    }
    device_commands_measurement_begin();
    esp_err_t err = s_cfg.uart_query(channel, cmd, extra, extra_len,
                                     expect_raw, response, timeout_ms);
    device_commands_measurement_end();
    if (err != ESP_OK) {
        return make_result(err, "UART ch%u query failed: %s",
                           channel, esp_err_to_name(err));
    }
    if (expect_raw > 0) {
        return make_result(ESP_OK, "UART ch%u: %u raw bytes",
                           channel, (unsigned)response->raw_len);
    }
    return make_result(ESP_OK, "UART ch%u: %u arrays received",
                       channel, response->array_count);
}

/* Ping: send wake byte (0xAA) to the AMBIT sensor, wait for ack (0x80).
 * Result is cached for 10 seconds to avoid hammering the bus.
 * Lua:  device.uart_ping(ch) → true/false
 * CLI:  ping_uart <ch>                                                */
cmd_result_t cmd_uart_ping(uint8_t channel, bool *connected)
{
    if (!s_initialized || s_cfg.uart_ping == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (channel >= UART_SENSOR_NUM_CHANNELS) {
        return make_result(ESP_ERR_INVALID_ARG, "invalid channel %u", channel);
    }
    device_commands_measurement_begin();
    esp_err_t err = s_cfg.uart_ping(channel, connected);
    device_commands_measurement_end();
    if (err != ESP_OK) {
        return make_result(err, "UART ch%u ping failed: %s",
                           channel, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u: %s",
                       channel + 1, *connected ? "connected" : "disconnected");
}

/* Report connection state of all 4 UART channels.
 * Lua:  device.uart_status() → string
 * CLI:  uart_status                                                   */
cmd_result_t cmd_uart_status(void)
{
    if (!s_initialized || s_cfg.uart_status == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    static const char *state_str[] = { "disconnected", "connected", "busy" };
    char buf[200];
    int pos = 0;
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        uart_sensor_state_t st = UART_SENSOR_DISCONNECTED;
        s_cfg.uart_status(ch, &st);
        int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                         "AMBIT%u:%s ", ch + 1,
                         (st < 3) ? state_str[st] : "?");
        if (n > 0 && pos + n < (int)sizeof(buf)) {
            pos += n;
        }
    }
    return make_result(ESP_OK, "%s", buf);
}

/* Generic ASCII line-oriented query — sends "<cmd><terminator>", reads one
 * line back, and discards the first line if it echoes the command verbatim.
 * Transport/diagnostic only: NEVER stores (schema-v2 rule — measurement
 * commands store, transport commands don't). See device_commands.h. */
cmd_result_t cmd_uart_text_query(uint8_t channel,
                                 const char *cmd, const char *terminator,
                                 uint32_t timeout_ms,
                                 char *out_resp, size_t resp_cap,
                                 size_t *resp_len)
{
    if (!s_initialized || s_cfg.uart_text_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (channel >= UART_SENSOR_NUM_CHANNELS) {
        return make_result(ESP_ERR_INVALID_ARG, "invalid channel %u", channel);
    }
    if (cmd == NULL || terminator == NULL || out_resp == NULL ||
        resp_len == NULL || resp_cap < 2) {
        return make_result(ESP_ERR_INVALID_ARG, "bad args");
    }

    int64_t t0_us = esp_timer_get_time();  /* monotonic, for echo-retry budget */
    *resp_len = 0;
    out_resp[0] = '\0';

    /* First attempt — full timeout budget. */
    device_commands_measurement_begin();
    esp_err_t err = s_cfg.uart_text_query(channel, cmd, terminator,
                                          out_resp, resp_cap, resp_len, timeout_ms);

    /* Echo handling: if the first line equals the sent cmd, throw it away and
     * read the next line with whatever budget is left. The line we just read
     * is already in out_resp without the terminator. */
    if (err == ESP_OK) {
        size_t cmd_len = strlen(cmd);
        if (*resp_len == cmd_len && memcmp(out_resp, cmd, cmd_len) == 0) {
            int64_t elapsed_us = esp_timer_get_time() - t0_us;
            int64_t remaining_ms = ((int64_t)timeout_ms * 1000 - elapsed_us) / 1000;
            if (remaining_ms <= 0) {
                err = ESP_ERR_TIMEOUT;
                *resp_len = 0;
                out_resp[0] = '\0';
            } else {
                /* Read another line — pass an empty cmd so nothing is re-sent. */
                *resp_len = 0;
                out_resp[0] = '\0';
                err = s_cfg.uart_text_query(channel, "", terminator,
                                            out_resp, resp_cap, resp_len,
                                            (uint32_t)remaining_ms);
            }
        }
    }
    device_commands_measurement_end();

    /* On hard error (other than timeout) propagate. */
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        return make_result(err, "uart_text_query ch%u failed: %s",
                           channel, esp_err_to_name(err));
    }

    if (err == ESP_ERR_TIMEOUT) {
        return make_result(ESP_ERR_TIMEOUT, "ch%u: no response within %ums",
                           channel, (unsigned)timeout_ms);
    }
    return make_result(ESP_OK, "ch%u: %u bytes", channel, (unsigned)*resp_len);
}

/* ── Typed Ambit commands ────────────────────────────────────────── *
 *
 * Each function wraps one ambit-1 ESP binary command (see ambit_protocol.h
 * for command IDs). The ambit-1 protocol is:
 *   Wake (0xAA×3) → Ack (0x80) → Header (0xA0) + 8-byte cmd [+extra]
 *   → CMD_DONE (0xA1) → [response] → [CMD_END (0xF0)]
 *
 * Four response patterns exist:
 *   ACK_ONLY  — cmds 1, 2, 10: CMD_DONE only, no CMD_END (config)
 *   RAW       — cmds 31-34:    CMD_DONE → N fixed bytes → CMD_END (query)
 *   FSM       — cmds 20, 21:   CMD_DONE → FSM handshake arrays → CMD_END (measurement)
 *   ACTION    — cmds 4-6, 17-18, 37: CMD_DONE → [work] → CMD_END (no data returned)
 * ────────────────────────────────────────────────────────────────── */

/* Helper: send an ack-only command (no response, no CMD_END) */
static cmd_result_t ambit_ack_only(uint8_t ch, const uint8_t cmd[8])
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     UART_QUERY_ACK_ONLY, &resp, 5000);
    uart_sensor_response_free(&resp);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u cmd %u failed: %s",
                           ch + 1, cmd[0], esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u cmd %u OK", ch + 1, cmd[0]);
}

/* Helper: send a command that returns CMD_END but no data (action) */
static cmd_result_t ambit_action(uint8_t ch, const uint8_t cmd[8],
                                 const uint8_t *extra, size_t extra_len,
                                 uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, extra, extra_len,
                                     0, &resp, timeout_ms);
    uart_sensor_response_free(&resp);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u cmd %u failed: %s",
                           ch + 1, cmd[0], esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u cmd %u OK", ch + 1, cmd[0]);
}

/* Per-channel AMBIT identity + config cache. Identity/calibration is fetched
 * once via cmd 33 (see ambit_info_fetch below); gains/currents are tracked here
 * at set-time. Written from the measurement (Lua) task, so no lock as long as
 * that stays the only writer. */
#define AMBIT_INFO_NUM_CH 4
static ambit_device_info_t s_ambit_info[AMBIT_INFO_NUM_CH];

/* Cmd 1 — Set photodetector gains on the ADPD6100.
 * Values 1-6 map to gain levels (0 = skip / keep current).
 * Must be called before cmd_ambit_config_detector() or cmd_ambit_run().
 * Lua:  device.ambit_set_gains(ch, fluo, fluoref, ir, irref, sun, leaf)
 * CLI:  (not exposed — use Lua)                                       */
cmd_result_t cmd_ambit_set_gains(uint8_t ch, uint8_t fluo, uint8_t fluoref,
                                  uint8_t ir, uint8_t irref,
                                  uint8_t sun, uint8_t leaf)
{
    uint8_t cmd[8] = { AMBIT_CMD_SET_GAINS, fluo, fluoref, ir, irref, sun, leaf, 0 };
    cmd_result_t r = ambit_ack_only(ch, cmd);
    /* Track at set-time — the AMBIT has no read-back, and we're the only setter,
     * so this stays authoritative for the event metadata. */
    if (r.status == ESP_OK && ch < AMBIT_INFO_NUM_CH) {
        s_ambit_info[ch].gains[0] = fluo;    s_ambit_info[ch].gains[1] = fluoref;
        s_ambit_info[ch].gains[2] = ir;      s_ambit_info[ch].gains[3] = irref;
        s_ambit_info[ch].gains[4] = sun;     s_ambit_info[ch].gains[5] = leaf;
        s_ambit_info[ch].gains_set = true;
    }
    return r;
}

/* Cmd 2 — Set LED drive currents (0-126).
 * i620 = 620nm pulsed, i720 = 720nm pulsed, ir = far-red DC.
 * Must be called before cmd_ambit_config_detector() or cmd_ambit_run().
 * Lua:  device.ambit_set_currents(ch, i620, i720, ir)                 */
cmd_result_t cmd_ambit_set_currents(uint8_t ch, uint8_t i620, uint8_t i720,
                                     uint8_t ir)
{
    uint8_t cmd[8] = { AMBIT_CMD_SET_CURRENTS, i620, i720, ir, 0, 0, 0, 0 };
    cmd_result_t r = ambit_ack_only(ch, cmd);
    if (r.status == ESP_OK && ch < AMBIT_INFO_NUM_CH) {
        s_ambit_info[ch].currents[0] = i620; s_ambit_info[ch].currents[1] = i720;
        s_ambit_info[ch].currents[2] = ir;
        s_ambit_info[ch].currents_set = true;
    }
    return r;
}

/* Cmd 10 — Apply stored gains and currents to the ADPD6100 detector.
 * Configures the detector into ARRAY_MODE1. Call after set_gains/set_currents,
 * or omit if cmd_ambit_run() auto-configures when mode differs.
 * Lua:  device.ambit_config_detector(ch)                              */
cmd_result_t cmd_ambit_config_detector(uint8_t ch)
{
    uint8_t cmd[8] = { AMBIT_CMD_CONFIG_DETECTOR, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_ack_only(ch, cmd);
}

/* Cmd 32 — Read leaf and chip temperature from the MLX90632 IR sensor.
 * Returns temperatures in Celsius (ambit sends int16 × 10, we divide).
 * Response: 4 bytes (2 × int16_t little-endian).
 * Lua:  device.ambit_get_temp(ch) → {leaf=float, chip=float}
 * CLI:  ambit_temp <ch>                                               */
cmd_result_t cmd_ambit_get_temp(uint8_t ch, float *leaf_temp, float *chip_temp)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_GET_TEMP, 0, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     AMBIT_RESP_TEMP_SIZE, &resp, 5000);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < AMBIT_RESP_TEMP_SIZE) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u get_temp failed", ch + 1);
    }
    int16_t t1, t2;
    memcpy(&t1, resp.raw + 0, 2);
    memcpy(&t2, resp.raw + 2, 2);
    if (leaf_temp) *leaf_temp = (float)t1 / 10.0f;
    if (chip_temp) *chip_temp = (float)t2 / 10.0f;
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u T=%.1fC chip=%.1fC",
                       ch + 1, (float)t1 / 10.0f, (float)t2 / 10.0f);
}

/* Cmd 31 — Read spectral channels from the AS7341 and compute PAR.
 * Response: 24 bytes = 10 × uint16 channels + 1 × float32 PAR.
 * Lua:  device.ambit_get_spec(ch) → {spec={10 ints}, par=float}
 * CLI:  ambit_spec <ch>                                               */
cmd_result_t cmd_ambit_get_spec(uint8_t ch, uint16_t spec[10], float *par)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_GET_SPEC, 0, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     AMBIT_RESP_SPEC_SIZE, &resp, 5000);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < AMBIT_RESP_SPEC_SIZE) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u get_spec failed", ch + 1);
    }
    /* spec[0..9] are uint16 channels, spec[10..11] hold a float (PAR) */
    if (spec) memcpy(spec, resp.raw, 20);
    if (par)  memcpy(par, resp.raw + 20, 4);
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u spectrum OK", ch + 1);
}

/* Cmd 34 — Extended temperature read: two leaf algorithms + chip + 4 raw
 * MLX90632 register values for diagnostics.
 * Response: 14 bytes (7 × int16_t: leaf*10, leaf1*10, chip*10, a1..a4).
 * Lua:  device.ambit_get_temp_raw(ch) → {leaf,leaf1,chip,raw={4 ints}}
 * CLI:  (not exposed — use Lua or ambit_temp for basic reading)       */
cmd_result_t cmd_ambit_get_temp_raw(uint8_t ch, float *leaf, float *leaf1,
                                     float *chip, int16_t raw[4])
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_GET_TEMP_RAW, 0, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     AMBIT_RESP_TEMP_RAW_SIZE, &resp, 5000);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < AMBIT_RESP_TEMP_RAW_SIZE) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u get_temp_raw failed", ch + 1);
    }
    int16_t vals[7];
    memcpy(vals, resp.raw, 14);
    if (leaf)  *leaf  = (float)vals[0] / 10.0f;
    if (leaf1) *leaf1 = (float)vals[1] / 10.0f;
    if (chip)  *chip  = (float)vals[2] / 10.0f;
    if (raw) {
        raw[0] = vals[3]; raw[1] = vals[4];
        raw[2] = vals[5]; raw[3] = vals[6];
    }
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u T=%.1f/%.1f/%.1fC",
                       ch + 1, (float)vals[0]/10.f, (float)vals[1]/10.f, (float)vals[2]/10.f);
}

/* Cmd 33 — Retrieve sensor identity/calibration data.
 *   info_type=1: ambit_calibration_t (~136 B) — name, MLX coefficients,
 *                ADPD offsets, actinic/spec coefficients, emissivity
 *   info_type=2: ambit_fw_info_t (~48 B) — FW version, MAC, build date
 *   info_type=3: ambit_metadata_t (~248 B) — GPS, altitude, user notes
 * Raw struct bytes are copied into `out`. Struct layouts defined in
 * ambit_protocol.h (must match ambit-1 nvs1.h on ESP32 Xtensa alignment).
 * Lua:  device.ambit_get_info(ch, type) → raw bytes string
 * CLI:  ambit_info <ch> <1|2|3>                                       */
cmd_result_t cmd_ambit_get_info(uint8_t ch, uint8_t info_type,
                                 uint8_t *out, size_t out_size, size_t *out_len)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    size_t expect = 0;
    switch (info_type) {
    case AMBIT_INFO_CALIBRATION: expect = sizeof(ambit_calibration_t); break;
    case AMBIT_INFO_FW:          expect = sizeof(ambit_fw_info_t);     break;
    case AMBIT_INFO_METADATA:    expect = sizeof(ambit_metadata_t);    break;
    default:
        return make_result(ESP_ERR_INVALID_ARG, "info_type must be 1-3");
    }

    uint8_t cmd[8] = { AMBIT_CMD_GET_INFO, info_type, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0, expect, &resp, 5000);
    if (err != ESP_OK || resp.raw == NULL) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u get_info(%u) failed", ch + 1, info_type);
    }
    size_t copy = resp.raw_len < out_size ? resp.raw_len : out_size;
    if (out && copy > 0) memcpy(out, resp.raw, copy);
    if (out_len) *out_len = resp.raw_len;
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u info(%u): %u bytes",
                       ch + 1, info_type, (unsigned)resp.raw_len);
}

/* ── Cached AMBIT device identity (deviceID / fw_version / cal_version) ──────
 * The cache + setters live above (near cmd_ambit_set_gains); below is the
 * one-time fetch that fills identity/calibration and announces it. */

/* Emit one DEVICE_INFO event (tag DEVICE_INFO, channel uart_<ch>, device =
 * ambit_name, cmd_raw "get_info") carrying the full calibration. Called once
 * per connection from ambit_info_fetch. Best-effort: a store failure must not
 * fail the identity fetch. */
static void ambit_emit_device_info(uint8_t ch, const ambit_device_info_t *e,
                                   const ambit_calibration_t *cal, bool have_cal)
{
    if (s_cfg.store_event == NULL || s_cfg.next_id == NULL) return;

    char payload[768];
    int o = snprintf(payload, sizeof payload,
        "{\"device_id\":\"%s\",\"fw\":\"%s\",\"cal_version\":\"%08lx\"",
        e->device_id, e->fw_version, (unsigned long)e->cal_version);
    if (have_cal && o > 0 && o < (int)sizeof payload) {
        o += snprintf(payload + o, sizeof payload - o, ",\"mlx_coef\":[");
        for (int i = 0; i < 14 && o > 0 && o < (int)sizeof payload; i++)
            o += snprintf(payload + o, sizeof payload - o, "%s%ld",
                          i ? "," : "", (long)cal->mlx_coef[i]);
        o += snprintf(payload + o, sizeof payload - o, "],\"adpd\":[");
        for (int i = 0; i < 6 && o > 0 && o < (int)sizeof payload; i++)
            o += snprintf(payload + o, sizeof payload - o, "%s%lu",
                          i ? "," : "", (unsigned long)cal->adpd[i]);
        o += snprintf(payload + o, sizeof payload - o,
            "],\"temp_offset\":%.4f,\"temp_slope\":%.4f,\"actinic_coef\":%.6f,"
            "\"spec_coef\":%.6f,\"act\":[%u,%u,%u,%u,%u],"
            "\"mlx_emissivity\":%.4f,\"sun_coef\":%.6f,\"tick_factor\":%.6f",
            (double)cal->temp_offset, (double)cal->temp_slope, (double)cal->actinic_coef,
            (double)cal->spec_coef, (unsigned)cal->act_50, (unsigned)cal->act_100,
            (unsigned)cal->act_150, (unsigned)cal->act_200, (unsigned)cal->act_250,
            (double)cal->mlx_emissivity, (double)cal->sun_coef, (double)cal->tick_factor);
    }
    if (o < 0 || o >= (int)sizeof payload) {
        ESP_LOGW(TAG, "AMBIT%u device_info payload truncated", ch + 1);
        return;
    }

    int64_t mid = 0;
    if (s_cfg.next_id(&mid) != ESP_OK) return;
    char chan[12];
    snprintf(chan, sizeof chan, "uart_%u", (unsigned)ch);
    measurement_event_desc_t d = {
        .measure_id   = mid,
        .channel      = chan,
        .device       = (e->ambit_name[0] != '\0') ? e->ambit_name : "ambit",
        .tag          = MEASUREMENT_TAG_DEVICE_INFO,
        .cmd_raw      = "get_info",
        .start_ms     = now_ms(),
        .end_ms       = now_ms(),
        .payload_json = payload,
    };
    if (s_cfg.store_event(&d) == ESP_OK) {
        notify_sync();
        ESP_LOGI(TAG, "AMBIT%u DEVICE_INFO stored (%s cal=%08lx)",
                 ch + 1, e->ambit_name, (unsigned long)e->cal_version);
    }
}

static esp_err_t ambit_info_fetch(uint8_t ch)
{
    /* FW info (MAC + version) is mandatory; calibration (→ cal_version) best-effort. */
    ambit_fw_info_t fw;
    size_t got = 0;
    cmd_result_t r = cmd_ambit_get_info(ch, AMBIT_INFO_FW, (uint8_t *)&fw, sizeof fw, &got);
    if (r.status != ESP_OK || got < sizeof fw) {
        return (r.status != ESP_OK) ? r.status : ESP_FAIL;
    }

    ambit_device_info_t e;
    memset(&e, 0, sizeof e);
    /* getEfuseMac() packs the 6-byte MAC little-endian (byte0 = low 8 bits). */
    uint64_t m = fw.mac;
    snprintf(e.device_id, sizeof e.device_id, "%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)(m & 0xFF), (unsigned)((m >> 8) & 0xFF), (unsigned)((m >> 16) & 0xFF),
             (unsigned)((m >> 24) & 0xFF), (unsigned)((m >> 32) & 0xFF), (unsigned)((m >> 40) & 0xFF));
    snprintf(e.fw_version, sizeof e.fw_version, "%u.%u.%u",
             (unsigned)fw.major, (unsigned)fw.minor, (unsigned)fw.batch);

    /* cal_version = CRC32 of the calibration struct → changes whenever the sensor
     * is recalibrated. The struct has no native version field. */
    ambit_calibration_t cal;
    size_t cgot = 0;
    bool have_cal = false;
    cmd_result_t cr = cmd_ambit_get_info(ch, AMBIT_INFO_CALIBRATION, (uint8_t *)&cal, sizeof cal, &cgot);
    if (cr.status == ESP_OK && cgot >= sizeof cal) {
        have_cal        = true;
        e.cal_version   = esp_rom_crc32_le(0, (const uint8_t *)&cal, sizeof cal);
        e.actinic_coef  = cal.actinic_coef;
        memcpy(e.ambit_name, cal.ambit_name, sizeof e.ambit_name - 1);
    }

    /* Preserve gains/currents tracked since the last (re)connect — the identity
     * fetch must not clobber them (they live in the same cache struct). */
    e.gains_set     = s_ambit_info[ch].gains_set;
    memcpy(e.gains, s_ambit_info[ch].gains, sizeof e.gains);
    e.currents_set  = s_ambit_info[ch].currents_set;
    memcpy(e.currents, s_ambit_info[ch].currents, sizeof e.currents);

    e.valid = true;
    s_ambit_info[ch] = e;

    /* Announce the freshly-connected sensor's identity + calibration once. */
    ambit_emit_device_info(ch, &e, &cal, have_cal);
    return ESP_OK;
}

cmd_result_t cmd_ambit_device_info(uint8_t ch, ambit_device_info_t *out)
{
    if (ch >= AMBIT_INFO_NUM_CH || out == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "device_info: bad channel/arg");
    }
    if (!s_ambit_info[ch].valid) {
        esp_err_t err = ambit_info_fetch(ch);
        if (err != ESP_OK) {
            memset(out, 0, sizeof *out);
            return make_result(err, "AMBIT%u device_info fetch failed", ch + 1);
        }
    }
    *out = s_ambit_info[ch];
    return make_result(ESP_OK, "AMBIT%u %s fw=%s cal=%08lx",
                       ch + 1, out->device_id, out->fw_version, (unsigned long)out->cal_version);
}

void cmd_ambit_device_info_invalidate(uint8_t ch)
{
    if (ch < AMBIT_INFO_NUM_CH) {
        /* Drop identity (re-fetch + re-announce next use) AND tracked gains/
         * currents — the reconnected AMBIT booted with its own defaults, unknown
         * to us until the script sets them again. */
        s_ambit_info[ch].valid        = false;
        s_ambit_info[ch].gains_set    = false;
        s_ambit_info[ch].currents_set = false;
    }
}

/* Cmd 21 — Run an array-mode measurement on the ADPD6100.
 * `run_arr` is a flat byte array of arr_len × 8 bytes. Each 8-byte line
 * encodes: line_type, ir_on, sample_num(H), sample_num(L), freq(H),
 * freq(L), actinic, subsampling. Max 16 lines.
 * The extra payload is sent before CMD_DONE (buffered in ambit RX FIFO).
 * Response: FSM handshake — up to 7 data arrays (env, fluor, fluoref,
 * sun, leaf, 730, 730ref), each as {index, uint32_t[], length}.
 * Typical duration: seconds to ~60s depending on sample count.
 * Lua:  device.ambit_run(ch, flat_table, led_persist, allow_int, timeout)
 * CLI:  (not exposed — use Lua scripts for measurement workflows)     */
cmd_result_t cmd_ambit_run(uint8_t ch, const uint8_t *run_arr, uint8_t arr_len,
                            uint8_t led_persist, bool allow_interrupt,
                            uart_sensor_response_t *response, uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (arr_len == 0 || arr_len > 16 || run_arr == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "arr_len must be 1-16");
    }
    uint8_t cmd[8] = { AMBIT_CMD_RUN, arr_len, led_persist,
                       (uint8_t)(allow_interrupt ? 1 : 0), 0, 0, 0, 0 };
    memset(response, 0, sizeof(*response));
    esp_err_t err = s_cfg.uart_query(ch, cmd, run_arr, (size_t)arr_len * 8,
                                     0, response, timeout_ms);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u run failed: %s",
                           ch + 1, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u run: %u arrays",
                       ch + 1, response->array_count);
}

/* Cmd 20 — Run a multi-phase fluorescence (MPF) measurement.
 * `length` = total measurement points (uint16, encoded as [1]<<7 | [2]),
 * `interval` = sampling interval, `change_act`/`act` = actinic control.
 * Response: FSM handshake data arrays (same as cmd 21).
 * Lua:  device.ambit_run_mpf(ch, length, interval, change_act, act, timeout)
 * CLI:  (not exposed — use Lua)                                       */
cmd_result_t cmd_ambit_run_mpf(uint8_t ch, uint16_t length, uint8_t interval,
                                bool change_act, uint8_t act,
                                uart_sensor_response_t *response, uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_RUN_MPF,
                       (uint8_t)(length >> 7), (uint8_t)(length & 0x7F),
                       interval, (uint8_t)(change_act ? 1 : 0), act, 0, 0 };
    memset(response, 0, sizeof(*response));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0, 0, response, timeout_ms);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u run_mpf failed: %s",
                           ch + 1, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u mpf: %u arrays",
                       ch + 1, response->array_count);
}

/* ── Parallel measurement protocol (trigger → poll → fetch) ─────────────────
 * Lets the host start a run on every AMBIT back-to-back and collect them
 * afterwards, instead of blocking the whole run per channel. The four C3s
 * measure concurrently; the host only ever holds the (single shared) bus for a
 * short trigger/poll/fetch transaction. These deliberately do NOT bracket the
 * measurement gate — the Lua orchestrator asserts it once across the cycle. */

/* Cmd 22 — Trigger an async (retained) run. Same payload as cmd 21; the ambit
 * acks CMD_DONE (ACK_ONLY) and then runs into its own buffers, staying silent
 * until FETCH. Keep timeout_ms short — this only covers wake + ack. */
cmd_result_t cmd_ambit_trigger(uint8_t ch, const uint8_t *run_arr, uint8_t arr_len,
                               uint8_t led_persist, bool allow_interrupt,
                               uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (arr_len == 0 || arr_len > 16 || run_arr == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "arr_len must be 1-16");
    }
    uint8_t cmd[8] = { AMBIT_CMD_RUN_START, arr_len, led_persist,
                       (uint8_t)(allow_interrupt ? 1 : 0), 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, run_arr, (size_t)arr_len * 8,
                                     UART_QUERY_ACK_ONLY, &resp, timeout_ms);
    uart_sensor_response_free(&resp);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u trigger failed: %s", ch + 1, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u triggered", ch + 1);
}

/* Cmd 23 — Poll async run state into *state (AMBIT_ASYNC_IDLE|DONE|ERROR). A
 * measuring ambit doesn't answer, so ESP_ERR_TIMEOUT here means "busy" — the
 * caller maps it. Keep timeout_ms short (a few wake retries) so a busy/locked
 * channel fails fast instead of spraying wake bytes at a measuring sensor. */
cmd_result_t cmd_ambit_poll(uint8_t ch, uint8_t *state, uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (state) *state = 0xFF;   /* unknown until the ambit answers */
    uint8_t cmd[8] = { AMBIT_CMD_STATUS, 0, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     AMBIT_RESP_STATUS_SIZE, &resp, timeout_ms);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < AMBIT_RESP_STATUS_SIZE) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u poll: no answer", ch + 1);
    }
    if (state) *state = resp.raw[0];
    uint8_t st = resp.raw[0];
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u state=%u", ch + 1, (unsigned)st);
}

/* Cmd 24 — Fetch the retained run result. The ambit streams its buffered arrays
 * back over the FSM exactly like cmd 21, so `response` is identical to
 * cmd_ambit_run's output. timeout_ms must cover the stream (scale with size). */
cmd_result_t cmd_ambit_fetch(uint8_t ch, uart_sensor_response_t *response,
                             uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_FETCH, 0, 0, 0, 0, 0, 0, 0 };
    memset(response, 0, sizeof(*response));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0, 0, response, timeout_ms);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u fetch failed: %s", ch + 1, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u fetch: %u arrays", ch + 1, response->array_count);
}

/* Cmd 5 — Blink the AS7341 LED for visual identification.
 * `ambit_id` 0-3 selects blink pattern, `intensity` 5-253 sets brightness.
 * Blocks until blink completes (a few seconds).
 * Lua:  device.ambit_blink(ch, id, intensity)
 * CLI:  ambit_blink <ch> <id> <intensity>                             */
cmd_result_t cmd_ambit_blink(uint8_t ch, uint8_t ambit_id, uint8_t intensity)
{
    uint8_t cmd[8] = { AMBIT_CMD_BLINK, ambit_id, intensity, 0, 0, 0, 0, 0 };
    return ambit_action(ch, cmd, NULL, 0, 10000);
}

/* Cmd 6 — Run ADPD6100 fluorescence offset calibration.
 * Measures dark baseline and stores offsets (adpd_lit, adpd_sun, adpd_leaf,
 * adpd_730, adpd_730r) in the ambit's NVS. Factory/maintenance command.
 * Blocks for several seconds.
 * Lua:  device.ambit_calibrate_baseline(ch)                           */
cmd_result_t cmd_ambit_calibrate_baseline(uint8_t ch)
{
    uint8_t cmd[8] = { AMBIT_CMD_CALIBRATE_BASELINE, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_action(ch, cmd, NULL, 0, 30000);
}

/* Cmd 4 — Actinic LED control and calibration.
 *   type=1: test actinics — ramps LED current (var), blocks ~6s
 *   type=2: set actinic coefficient (float packed in cmd[3..6])
 *   type=4: set spectral coefficient
 *   type=5: pulse LED at `var` current for `var2`×100 ms
 * Factory/calibration command.
 * Lua:  device.ambit_actinic(ch, type, var, var2)                     */
cmd_result_t cmd_ambit_actinic(uint8_t ch, uint8_t type, uint8_t var, uint8_t var2)
{
    uint8_t cmd[8] = { AMBIT_CMD_ACTINIC, type, var, var2, 0, 0, 0, 0 };
    return ambit_action(ch, cmd, NULL, 0, 15000);
}

/* Cmd 37 — Write metadata (GPS coordinates, altitude, user notes) to
 * the ambit's NVS. `metadata` should be a serialised ambit_metadata_t
 * (~248 bytes). The struct's eof_mark must be 2025 for the ambit to
 * accept the write. Data is sent as `extra` before CMD_DONE; the ambit
 * reads it from its UART RX FIFO after acknowledging.
 * Lua:  device.ambit_set_metadata(ch, metadata_string)                */
cmd_result_t cmd_ambit_set_metadata(uint8_t ch, const uint8_t *metadata, size_t len)
{
    uint8_t cmd[8] = { AMBIT_CMD_SET_METADATA, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_action(ch, cmd, metadata, len, 5000);
}

/* ── AMBIT OTA-over-UART (cmds 25-28) ───────────────────────────────────────
 * Each command sends its 8-byte header (+ optional chunk payload) and reads the
 * AMBIT's 1-byte status reply (DONE + status + END). The ambit_ota component
 * orchestrates begin → data* → end; these only frame one exchange. */

static uint16_t ambit_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Send one OTA command (cmd[8] + optional extra) and read the 1-byte status. */
static cmd_result_t ambit_ota_cmd(uint8_t ch, const uint8_t cmd[8],
                                  const uint8_t *extra, size_t extra_len,
                                  uint8_t *status, uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, extra, extra_len,
                                     1 /* 1-byte status */, &resp, timeout_ms);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < 1) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u OTA cmd %u: no answer", ch + 1, cmd[0]);
    }
    uint8_t st = resp.raw[0];
    if (status) *status = st;
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u OTA cmd %u status=%u", ch + 1, cmd[0], st);
}

cmd_result_t cmd_ambit_ota_begin(uint8_t ch, uint32_t image_size, uint8_t *status)
{
    uint8_t cmd[8] = { AMBIT_CMD_OTA_BEGIN,
                       (uint8_t)image_size,         (uint8_t)(image_size >> 8),
                       (uint8_t)(image_size >> 16), (uint8_t)(image_size >> 24),
                       0, 0, 0 };
    /* begin() erases the target OTA partition (~hundreds of ms) — allow time. */
    return ambit_ota_cmd(ch, cmd, NULL, 0, status, 15000);
}

cmd_result_t cmd_ambit_ota_data(uint8_t ch, uint16_t seq,
                                const uint8_t *chunk, uint8_t len, uint8_t *status)
{
    if (chunk == NULL || len == 0 || len > AMBIT_OTA_CHUNK_MAX) {
        return make_result(ESP_ERR_INVALID_ARG, "OTA chunk len must be 1-%d", AMBIT_OTA_CHUNK_MAX);
    }
    uint8_t cmd[8] = { AMBIT_CMD_OTA_DATA, len,
                       (uint8_t)seq, (uint8_t)(seq >> 8), 0, 0, 0, 0 };
    uint8_t extra[AMBIT_OTA_CHUNK_MAX + 2];
    memcpy(extra, chunk, len);
    uint16_t crc = ambit_crc16_ccitt(chunk, len);
    extra[len]     = (uint8_t)crc;
    extra[len + 1] = (uint8_t)(crc >> 8);
    return ambit_ota_cmd(ch, cmd, extra, (size_t)len + 2, status, 5000);
}

cmd_result_t cmd_ambit_ota_end(uint8_t ch, uint8_t *status)
{
    uint8_t cmd[8] = { AMBIT_CMD_OTA_END, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_ota_cmd(ch, cmd, NULL, 0, status, 10000);
}

cmd_result_t cmd_ambit_ota_abort(uint8_t ch, uint8_t *status)
{
    uint8_t cmd[8] = { AMBIT_CMD_OTA_ABORT, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_ota_cmd(ch, cmd, NULL, 0, status, 5000);
}

cmd_result_t cmd_ambit_ota_confirm(uint8_t ch, uint8_t *status)
{
    uint8_t cmd[8] = { AMBIT_CMD_OTA_CONFIRM, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_ota_cmd(ch, cmd, NULL, 0, status, 5000);
}

