/*
 * power_protect.c — target glue for the brownout protection state machine
 * (contract: include/power_protect.h; decisions: power_protect_core.c).
 *
 * NVS namespace "pwr_protect", key "state": the pp_encode blob (113 B). Written
 * ONLY when the core says persist — a state transition, a recorded brownout
 * (at most one per boot), force, clear. Never per read, never per heartbeat:
 * a flash write per 5 s charger read would wear the NVS sector for nothing.
 * RTC_NOINIT is not enough on its own: a brownout is a power-on-class reset,
 * exactly the reset this module exists for, and it clears RTC memory.
 */
#include "power_protect.h"

#include <string.h>
#include <time.h>

#include "clock_trust.h"
#include "diag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define TAG "power_protect"
#define PP_NVS_NS  "pwr_protect"
#define PP_NVS_KEY "state"

static SemaphoreHandle_t s_mtx;
static StaticSemaphore_t s_mtx_storage;
static pp_core_t s_core;                 /* under s_mtx */
/* Published copy of s_core.p.state for lock-free readers (sched_runner,
 * ota_update, the pong on the esp-mqtt task). A byte store is atomic here. */
static volatile uint8_t s_state_pub = PP_STATE_NORMAL;
static portMUX_TYPE s_skip_mux = portMUX_INITIALIZER_UNLOCKED;
static pp_skips_t s_skips;               /* under s_skip_mux */

/* Valid = a UTC the brownout history may compare across boots. SNTP outranks
 * everything. An RTC-sourced clock counts only when nothing made it suspect
 * (OSF / read disagreement this boot) and it is past the 2024 floor. A clock
 * held up by the persisted high-water mark ("hwm", after a power loss stopped
 * the RTC) is a lower bound, not the time: never valid here. */
static pp_clock_t clock_now(void)
{
    const char *src = "rtc";
    bool suspect = false;
    clock_trust_get_status(&src, &suspect);
    const time_t now = time(NULL);
    const bool plausible = (long long)now >= CLOCK_TRUST_VALID_FLOOR_S;
    const bool trusted = strcmp(src, "sntp") == 0 || (strcmp(src, "rtc") == 0 && !suspect);
    return (pp_clock_t){ .valid = plausible && trusted, .utc = (int64_t)now };
}

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t persist(const pp_persist_t *p)
{
    uint8_t blob[PP_BLOB_LEN];
    if (pp_encode(p, blob, sizeof blob) != PP_BLOB_LEN) return ESP_FAIL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(PP_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, PP_NVS_KEY, blob, sizeof blob);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Under s_mtx. One ESP_LOGE per state transition: v2.18.0's diag hook turns
 * it into an ambyte.log/1 record, so the cloud sees every change with its
 * reason even when no heartbeat lands in between. */
static esp_err_t apply(const pp_event_t *ev)
{
    s_state_pub = s_core.p.state;
    if (ev->changed) {
        ESP_LOGE(TAG, "power_protect: %s -> %s reason=%s", pp_state_name(ev->from),
                 pp_state_name(ev->to), pp_reason_name(ev->reason));
    }
    if (!ev->persist) return ESP_OK;
    const esp_err_t err = persist(&s_core.p);
    if (err != ESP_OK) {
        /* RAM keeps the new state for this boot; only the next boot would
         * miss it. Warn (not error): the transition line already went out. */
        ESP_LOGW(TAG, "state not persisted: %s", esp_err_to_name(err));
    }
    return err;
}

static bool lock(void)
{
    return s_mtx != NULL && xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_mtx);
}

void power_protect_boot(void)
{
    if (s_mtx != NULL) return;   /* once per boot */
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_storage);
    pp_core_init(&s_core);

    nvs_handle_t h;
    if (nvs_open(PP_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t blob[PP_BLOB_LEN];
        size_t len = sizeof blob;
        const esp_err_t err = nvs_get_blob(h, PP_NVS_KEY, blob, &len);
        nvs_close(h);
        if (err == ESP_OK && !pp_decode(&s_core.p, blob, len)) {
            /* Unreadable state: start at normal rather than guess; this boot's
             * own brownout (if any) still protects it below. */
            ESP_LOGW(TAG, "persisted state unreadable (%u B) - starting at normal", (unsigned)len);
        } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "persisted state read failed: %s - starting at normal", esp_err_to_name(err));
        }
    }

    const bool brownout = esp_reset_reason() == ESP_RST_BROWNOUT;
    const pp_clock_t clk = clock_now();
    lock();
    const pp_event_t ev = pp_on_boot(&s_core, brownout, diag_boot_count(), clk);
    (void)apply(&ev);
    const uint32_t recent = pp_brownouts_6h(&s_core, clk, diag_boot_count());
    unlock();
    ESP_LOGW(TAG, "boot: state=%s reason=%s forced=%d brownouts_6h=%lu (reset %s, clock %s)",
             pp_state_name((pp_state_t)s_core.p.state), pp_reason_name((pp_reason_t)s_core.p.reason),
             s_core.p.forced ? 1 : 0, (unsigned long)recent, brownout ? "brownout" : "other",
             clk.valid ? "valid" : "not valid");
}

void power_protect_on_power_read(power_read_reason_t reason, const power_reading_t *p,
                                 bool fresh_present)
{
    if (!lock()) return;   /* before boot: nothing to protect yet */
    const pp_event_t ev = pp_on_read(&s_core, reason, p, fresh_present, mono_ms(), clock_now());
    (void)apply(&ev);
    unlock();
}

pp_state_t power_protect_state(void)
{
    return (pp_state_t)s_state_pub;
}

bool power_protect_blocks_flash(void)
{
    return pp_blocks_flash(power_protect_state());
}

bool power_protect_blocks_ambit(void)
{
    return pp_blocks_ambit(power_protect_state());
}

void power_protect_note_skip(power_protect_skip_t what)
{
    taskENTER_CRITICAL(&s_skip_mux);
    uint32_t *n = what == POWER_PROTECT_SKIP_FLASH_RUN ? &s_skips.flash_runs
                : what == POWER_PROTECT_SKIP_AMBIT_RUN ? &s_skips.ambit_runs
                : &s_skips.ota;
    if (*n < UINT32_MAX) (*n)++;
    taskEXIT_CRITICAL(&s_skip_mux);
}

static pp_skips_t skips_snapshot(void)
{
    taskENTER_CRITICAL(&s_skip_mux);
    const pp_skips_t s = s_skips;
    taskEXIT_CRITICAL(&s_skip_mux);
    return s;
}

void power_protect_get_status(power_protect_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->exit_in_s = -1;
    out->skipped = skips_snapshot();
    if (!lock()) {
        out->state = PP_STATE_NORMAL;
        out->reason = PP_REASON_NONE;
        return;
    }
    out->state = (pp_state_t)s_core.p.state;
    out->reason = (pp_reason_t)s_core.p.reason;
    out->forced = s_core.p.forced;
    out->since_utc = s_core.p.since_utc;
    out->brownouts_6h = pp_brownouts_6h(&s_core, clock_now(), diag_boot_count());
    const int64_t left = pp_window_remaining_ms(&s_core, mono_ms());
    out->exit_in_s = left < 0 ? -1 : left / 1000;
    unlock();
}

int power_protect_render_json(char *buf, size_t cap)
{
    const pp_skips_t skips = skips_snapshot();
    if (!lock()) {
        pp_persist_t normal;
        memset(&normal, 0, sizeof normal);
        return pp_render_json(buf, cap, &normal, 0, &skips);
    }
    const pp_persist_t p = s_core.p;
    const uint32_t recent = pp_brownouts_6h(&s_core, clock_now(), diag_boot_count());
    unlock();
    return pp_render_json(buf, cap, &p, recent, &skips);
}

esp_err_t power_protect_force(pp_state_t state)
{
    if (state != PP_STATE_NO_FLASH && state != PP_STATE_HOLD) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    const pp_event_t ev = pp_force(&s_core, state, clock_now());
    if (!ev.changed && ev.persist) {
        /* Same state, now forced: not a transition, but worth a line. */
        ESP_LOGW(TAG, "power_protect: %s now forced", pp_state_name(state));
    }
    const esp_err_t err = apply(&ev);
    unlock();
    return err;
}

esp_err_t power_protect_clear(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    const pp_event_t ev = pp_clear(&s_core, clock_now());
    if (!ev.changed && ev.persist) {
        ESP_LOGW(TAG, "power_protect: brownout history cleared");
    }
    const esp_err_t err = apply(&ev);
    unlock();
    return err;
}
