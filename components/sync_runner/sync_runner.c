#include "sync_runner.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "clock_trust.h"
#include "device_commands.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fleet_jitter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ota_update.h"
#include "timezone.h"

#define TAG "sync_runner"

/* Clock-validity floor (2024-01-01 UTC). Below this the system clock was never
 * set (RTC invalid and no NVS flash_time) — publishing would land events in
 * 1970 date partitions in the cloud, so the drain is gated until the clock is
 * plausible. Stores are NOT gated (payload-v2 decision: gate publish only);
 * events queue with whatever ticks they have. */
#define SYNC_CLOCK_FLOOR_S 1704067200LL

/* ── 1.0.6 self-healing policy thresholds ────────────────────────────────
 * These stay compile-time constants for this release: no safe runtime config
 * write path exists yet. Keeping the policy together makes the field tradeoffs
 * auditable and prevents one guard's units drifting from another's. */
#define SYNC_NIGHT_WINDOW_START_MIN       (2U * 60U)       /* local 02:00 */
#define SYNC_NIGHT_WINDOW_END_MIN         (4U * 60U)       /* local 04:00, exclusive */
#define SYNC_NIGHT_JITTER_SPAN_MIN        90U               /* spread fleet through first 90 min */
#define SYNC_NIGHT_MIN_UPTIME_S           (6LL * 60 * 60)  /* never reboot a freshly started unit */
#define SYNC_NIGHT_CLOCK_FALLBACK_S       (26LL * 60 * 60) /* clockless units still heal daily-ish */
#define SYNC_CONN_ERROR_THRESHOLD         30U               /* sustained churn, not one bad link */
#define SYNC_CONN_ERROR_WINDOW_S          (2U * 60U * 60U) /* rolling two-hour incident window */
#define SYNC_CONN_PUBLISH_STALE_MS         (30LL * 60 * 1000) /* churn matters only if delivery stopped */
#define SYNC_CONN_MIN_UPTIME_S            (12LL * 60 * 60) /* avoid rebooting setup/site outages */
#define SYNC_CONN_REBOOT_GUARD_S          (6LL * 60 * 60)  /* persistent NVS anti-loop latch */
#define SYNC_MEM_LARGEST_MIN_BYTES        (16U * 1024U)     /* internal DRAM allocation backstop */
#define SYNC_MEM_LOW_SAMPLES              10U               /* ten consecutive one-minute samples */
#define SYNC_MEM_MIN_UPTIME_S             (6LL * 60 * 60)  /* steady low pools cannot boot-loop */
#define SYNC_MEM_REBOOT_GUARD_S           (6LL * 60 * 60)  /* independent persistent anti-loop */
#define SYNC_WD_TICK_MS                   60000U             /* evaluate periodic guards once a minute */
#define SYNC_TIME_HWM_INTERVAL_MS          (60U * 60U * 1000U) /* durable rollback floor once/hour */
#define SYNC_WD_TIMEOUT_MS                (60LL * 60 * 1000) /* legacy: 1 h without PUBACK while due */
#define SYNC_PUBACK_MIN_UPTIME_S          (60LL * 60)       /* the 1 h observation period itself */
#define SYNC_PUBACK_REBOOT_GUARD_S        (2LL * 60 * 60)  /* fast recovery, independent of churn */
#define SYNC_CLOCKLESS_MEAS_ESCALATE_S    (50LL * 60 * 60) /* two fallback nights without a clock */

/* NVS keys are <= ESP-IDF's 15-character key limit. The public getter hides
 * these storage names from ticket 03's telemetry composition. */
#define SYNC_MAINT_NVS_NS                 "maint"
#define SYNC_MAINT_KEY_REBOOT_DAY         "last_reboot_day"
#define SYNC_MAINT_KEY_WD_REASON          "last_wd_reason"
#define SYNC_MAINT_KEY_CONN_REBOOT        "last_conn_wd"
#define SYNC_MAINT_KEY_MEM_REBOOT         "last_mem_wd"
#define SYNC_MAINT_KEY_PUBACK_REBOOT      "last_puback_wd"
#define SYNC_MAINT_KEY_BOOT_EPOCH         "boot_epoch"
#define SYNC_MAINT_KEY_REASON_BOOT        "wd_reason_boot"
#define SYNC_MAINT_KEY_MEAS_VETO_DAY      "meas_veto_day"
#define SYNC_MAINT_KEY_MEAS_VETO_COUNT    "meas_veto_cnt"
#define SYNC_WD_REASON_NIGHTLY            "nightly"
#define SYNC_WD_REASON_CONN               "conn"
#define SYNC_WD_REASON_MEM                "mem"
#define SYNC_WD_REASON_NOPUBACK            "nopuback"

/* Stagger the first drain past boot-time noise (Wi-Fi join, TLS, DB boot scan). */
#define SYNC_RUNNER_START_DELAY_MS  10000U
/* The first pass additionally waits for app_main's boot-complete signal (CLI up,
 * AMBIT auto-flash done, Lua started) so the backlog drain can never compete
 * with the startup sequence — the fixed stagger alone was decoupled from the
 * remaining boot work and a large backlog starved the prio-2 console. Capped so
 * a wedged boot step can't silence the publisher forever. */
#define SYNC_RUNNER_BOOT_WAIT_MAX_MS (5U * 60U * 1000U)
#define SYNC_RUNNER_BOOT_POLL_MS     500U
/* Fallback re-check when nothing notifies us — catches power-gate openings
 * (battery→external power, which is not a store event) and any missed wake.
 * Most drains are notification-driven; this is just the safety heartbeat. */
#define SYNC_RUNNER_FALLBACK_MS     30000U
#define SYNC_RUNNER_TASK_NAME    "sync_runner"
/* 8 KiB matches lua_runner. cmd_mqtt_publish_next_event heap-allocates the
 * envelope; event_log claim reads as much as the active record cap (64 KiB
 * normally) into its one-time PSRAM-backed s_line buffer. Neither scales this
 * task's stack with record size. */
#define SYNC_RUNNER_TASK_STACK   8192
#define SYNC_RUNNER_TASK_PRIO    3   /* below lua_runner (5), above idle */

/* ── Connectivity / liveness watchdog ────────────────────────────────────
 * A device that SHOULD be publishing (external power, valid clock, events
 * pending) but lands no PUBACK for a long stretch is stuck in a way the normal
 * retry paths can't clear on their own: the Wi-Fi manager has permanently given
 * up reconnecting, an in-flight slot is wedged, or the publisher task died. The
 * only universal recovery for an unattended remote unit is a reboot, which
 * re-arms Wi-Fi, re-opens MQTT, and rebuilds any lost state (the SD backlog and
 * NVS cursor survive, so nothing is lost). Runs as a SEPARATE task from the drain
 * so it also catches a wedged/dead drain task. */
#define SYNC_WD_TASK_NAME     "sync_wdog"
/* The canonical TELEMETRY builder and script/device health snapshots sit on
 * this path. A stack overflow here would take out the task that performs the
 * self-healing reboots, so the allocation is deliberately conservative;
 * confirm the high-water mark on hardware before trimming. */
#define SYNC_WD_TASK_STACK    7680
#define SYNC_WD_TASK_PRIO     2                      /* mostly sleeps */

/* Once the byte/slot window binds, yield briefly while the MQTT task delivers
 * completions. Successful publish iterations use taskYIELD() only: the point of
 * the window is to fill one connection without a fixed 100-ms serial cadence. */
#define SYNC_RUNNER_WINDOW_WAIT_MS 10U
/* An in-flight slot held longer than this had its PUBACK lost/expired (or the
 * broker dropped without notice while Wi-Fi stayed up): reap it so the event
 * re-publishes instead of the slot wedging the drain permanently. Well above any
 * real PUBACK round-trip and above esp-mqtt's ~30 s outbox-expiry default. */
#define SYNC_RUNNER_INFLIGHT_MAX_MS 60000

static TaskHandle_t s_task_handle = NULL;
static TaskHandle_t s_wd_task = NULL;
/* STATUS heartbeat period (s); 0 = disabled. Set once at start. */
static uint32_t     s_heartbeat_s = 0;
/* Boot-complete latch — see SYNC_RUNNER_BOOT_WAIT_MAX_MS. */
static volatile bool s_boot_complete = false;
/* Persisted boot generation. A watchdog reason is visible only when it targets
 * this boot, preventing old causes from being reported after a later reset. */
static uint32_t s_boot_epoch = 0;
static volatile bool s_wd_armed = false;
/* Composition-root probe for the global maintenance interlock shared by OTA,
 * scripts, and AMBIT flash. NULL is safe during component startup/tests. */
static bool (*s_maintenance_probe)(void) = NULL;

void sync_runner_set_maintenance_probe(bool (*probe)(void))
{
    s_maintenance_probe = probe; /* configured once by app_main before tasks run */
}

static bool maintenance_active(void)
{
    bool (*probe)(void) = s_maintenance_probe;
    return probe != NULL && probe();
}

void sync_runner_boot_complete(void)
{
    s_boot_complete = true;
    if (s_wd_task != NULL) xTaskNotifyGive(s_wd_task); /* emit boot STATUS now */
}

/* Wake the drain task. Registered as the device_commands store/gate-end
 * notifier; also safe to call directly. No-op until the task exists. */
void sync_runner_notify(void)
{
    TaskHandle_t h = s_task_handle;
    if (h != NULL) {
        xTaskNotifyGive(h);
    }
}

/* Sync gate: normally pause publishing only while a raw sensor transaction is
 * on the wire, so an already-open MQTT connection can drain during the rest of
 * a measurement routine. The legacy escape hatch restores the full measurement
 * window gate for field rollback. The external-power gate remains unchanged.
 * Events stay PENDING while either selected gate is closed. Weak so a future
 * power_monitor can override. */
__attribute__((weak)) bool sync_runner_is_allowed(void)
{
#if AMBYTE_PUBLISH_GATE_LEGACY
    bool sensor_gate_open = !device_commands_measurement_active();
#else
    bool sensor_gate_open = !device_commands_publish_hold_active();
#endif
    return sensor_gate_open && device_commands_publish_power_ok();
}

/* Publish pending events back-to-back until the queue drains or the gate closes.
 * One measure_id remains one MQTT message, but up to the independent slot/byte
 * ceilings may await PUBACK concurrently. */
static void sync_runner_drain(void)
{
    /* Heap snapshot at each drain entry — the "heap:" line referenced in
     * sdkconfig.defaults. Claims can read a full 64-KiB record from SD, but that
     * write lands in event_log's reusable PSRAM buffer, not here. The envelope
     * and MQTT outbox still scale to the record while TLS needs internal/DMA
     * headroom, so these capability-specific numbers govern the publish gate. */
    size_t window_slots = 0, window_bytes = 0;
    device_commands_window_status(&window_slots, &window_bytes);
    ESP_LOGI(TAG, "heap: 8bit free=%u largest=%u; internal free=%u largest=%u; dma free=%u largest=%u; window=%u/%u slots %u/%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)window_slots, (unsigned)PUBLISH_WINDOW_SLOTS,
             (unsigned)window_bytes, (unsigned)PUBLISH_WINDOW_BYTES);

    while (1) {
        /* ACK/error/disconnect callbacks run on MQTT/Wi-Fi event tasks and only
         * enqueue completions. Apply them here, before inspecting the RAM latch
         * or claiming another record. This single-task ordering is what keeps a
         * deferred PENDING transition from racing the next claim. A gate pauses
         * new publishes, never completion processing. */
        esp_err_t ack_err = cmd_process_pending_acks();
        if (ack_err != ESP_OK) {
            ESP_LOGW(TAG, "deferred ack processing blocked (%s)", esp_err_to_name(ack_err));
            return;
        }

        /* Reap one expired slot per pass. Because this is the sync-runner task,
         * mark_event_pending remains on the sole event-log mutation boundary.
         * The next loop reaps another if several crossed 60 s together. */
        if (device_commands_reap_stale_inflight(SYNC_RUNNER_INFLIGHT_MAX_MS)) {
            continue;
        }
        if (!sync_runner_is_allowed()) return;

        cmd_result_t res = cmd_mqtt_publish_next_event();
        if (res.status == ESP_OK) {
            ESP_LOGI(TAG, "%s", res.message);
            taskYIELD();
        } else if (res.status == ESP_ERR_INVALID_STATE) {
            /* Slot count, exact envelope bytes, raw-record bytes, or a
             * mid-window frontier guard can temporarily bind. Keep consuming
             * completions at 10-ms granularity; no new claim can pass a reverted
             * hole because event_log returns it first. */
            device_commands_window_status(&window_slots, &window_bytes);
            if (window_slots == 0) {
                ESP_LOGW(TAG, "window admission blocked with no MQTT slots: %s", res.message);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(SYNC_RUNNER_WINDOW_WAIT_MS));
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

/* ── Watchdog implementation ─────────────────────────────────────────────── */
static volatile bool s_wd_test = false;   /* one-shot: force a zero-timeout eval */

static esp_err_t maint_read_i32(const char *key, int32_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SYNC_MAINT_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_i32(h, key, out);
    nvs_close(h);
    return err;
}

static esp_err_t maint_read_i64(const char *key, int64_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SYNC_MAINT_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_i64(h, key, out);
    nvs_close(h);
    return err;
}

/* Advance the persistent boot generation before either watchdog task starts.
 * A self-reboot records the generation it expects after esp_restart(); ordinary
 * later boots therefore make the old reason stale without erasing diagnostics. */
static esp_err_t maint_begin_boot_epoch(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SYNC_MAINT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint32_t previous = 0;
    err = nvs_get_u32(h, SYNC_MAINT_KEY_BOOT_EPOCH, &previous);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    uint32_t current = previous == UINT32_MAX ? 1U : previous + 1U;
    if (err == ESP_OK) err = nvs_set_u32(h, SYNC_MAINT_KEY_BOOT_EPOCH, current);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_boot_epoch = current;
    return err;
}

esp_err_t sync_runner_get_boot_epoch(uint32_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_boot_epoch == 0) return ESP_ERR_INVALID_STATE;
    *out = s_boot_epoch;
    return ESP_OK;
}

bool sync_runner_watchdog_armed(void)
{
    return s_wd_armed;
}

esp_err_t sync_runner_get_last_wd_reboot_reason(char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(SYNC_MAINT_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    uint32_t reason_boot = 0;
    err = nvs_get_u32(h, SYNC_MAINT_KEY_REASON_BOOT, &reason_boot);
    size_t len = out_cap;
    if (err == ESP_OK) err = nvs_get_str(h, SYNC_MAINT_KEY_WD_REASON, out, &len);
    nvs_close(h);
    if (err == ESP_OK && (s_boot_epoch == 0 || reason_boot != s_boot_epoch)) {
        out[0] = '\0';
        return ESP_ERR_NVS_NOT_FOUND; /* reason belongs to an older boot */
    }
    return err;
}

/* Atomically persist every latch needed by a self-reboot. Refuse to restart if
 * NVS cannot commit: rebooting without the day/anti-loop state would turn a
 * recoverable storage failure into a reboot storm. */
static esp_err_t maint_latch_reboot(const char *reason, bool write_day,
                                    int32_t day, const char *guard_key,
                                    int64_t now_s)
{
    if (s_boot_epoch == 0) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SYNC_MAINT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, SYNC_MAINT_KEY_WD_REASON, reason);
    uint32_t reason_boot = s_boot_epoch == UINT32_MAX ? 1U : s_boot_epoch + 1U;
    if (err == ESP_OK) {
        err = nvs_set_u32(h, SYNC_MAINT_KEY_REASON_BOOT, reason_boot);
    }
    if (err == ESP_OK && write_day) {
        err = nvs_set_i32(h, SYNC_MAINT_KEY_REBOOT_DAY, day);
        /* A completed nightly cycle starts a fresh two-night escalation budget. */
        if (err == ESP_OK) err = nvs_set_i32(h, SYNC_MAINT_KEY_MEAS_VETO_COUNT, 0);
    }
    if (err == ESP_OK && guard_key != NULL) {
        err = nvs_set_i64(h, guard_key, now_s);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* The shared helper preserves the original full-MAC FNV-1a mapping. Reboot
 * policy keeps ownership of its warning and zero-minute failure fallback. */
static uint32_t nightly_jitter_minutes(void)
{
    uint32_t jitter_min = 0;
    esp_err_t err = fleet_jitter_slot_for_sta_mac(SYNC_NIGHT_JITTER_SPAN_MIN,
                                                   &jitter_min);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA MAC unavailable for reboot jitter: %s — using 0 min",
                 esp_err_to_name(err));
        return 0;
    }
    return jitter_min;
}

static bool watchdog_guard_allows(const char *key, const char *label,
                                  int64_t guard_s, time_t now_s,
                                  int64_t uptime_s)
{
    int64_t last_s = 0;
    esp_err_t err = maint_read_i64(key, &last_s);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_NOT_INITIALIZED) return true;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s watchdog latch read failed: %s — holding reboot",
                 label, esp_err_to_name(err));
        return false;
    }

    /* With a valid wall clock, the NVS epoch provides the cross-boot guard. A
     * clockless boot already has an uptime gate; current-boot uptime is the only
     * meaningful conservative comparison until RTC/flash-time becomes valid. */
    if (now_s < (time_t)SYNC_CLOCK_FLOOR_S) {
        return uptime_s >= guard_s;
    }
    if (last_s < SYNC_CLOCK_FLOOR_S) {
        return uptime_s >= guard_s;
    }
    if ((int64_t)now_s < last_s) {
        /* A far-future timestamp otherwise disables this watchdog silently until
         * wall time catches up. Surface the bogus latch loudly for field triage. */
        ESP_LOGW(TAG, "%s watchdog latch is %lld s in the future — holding reboot",
                 label, (long long)(last_s - (int64_t)now_s));
        return false;
    }
    return ((int64_t)now_s - last_s) >= guard_s;
}

static bool self_reboot(const char *reason, const char *detail,
                        bool write_day, int32_t day, const char *guard_key,
                        bool write_latch)
{
    if (write_latch) {
        esp_err_t err = maint_latch_reboot(reason, write_day, day, guard_key,
                                           (int64_t)time(NULL));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SELF-REBOOT BLOCKED (%s): NVS latch failed: %s",
                     reason, esp_err_to_name(err));
            return false;
        }
    }

    /* Persist a final TELEMETRY snapshot for post-boot delivery. It is best-effort:
     * production reason/day/guard latches above are the hard anti-loop
     * requirement. Test mode intentionally writes none. The app's registered
     * shutdown handler flushes event_log + SD during esp_restart(). */
    cmd_result_t sr = cmd_store_status_event();
    if (sr.status != ESP_OK && sr.status != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "pre-reboot TELEMETRY failed: %s", sr.message);
    }
    ESP_LOGE(TAG, "SELF-REBOOT reason=%s: %s", reason, detail);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return true; /* unreachable; keeps the failure-return call sites explicit */
}

/* Convert YYYYMMDD into a monotonic civil-day index. Calendar subtraction on
 * the packed integer fails at month/year boundaries; this compact Gregorian
 * conversion keeps the "consecutive eligible nights" escalation exact. */
static bool calendar_day_index(int32_t yyyymmdd, int64_t *out)
{
    int year = yyyymmdd / 10000;
    unsigned month = (unsigned)((yyyymmdd / 100) % 100);
    unsigned day = (unsigned)(yyyymmdd % 100);
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31) return false;
    year -= month <= 2;
    int era = year / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned adjusted_month = month > 2 ? month - 3U : month + 9U;
    unsigned doy = (153U * adjusted_month + 2U) / 5U
                 + day - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    *out = (int64_t)era * 146097LL + (int64_t)doe;
    return true;
}

/* Record at most one measurement veto per local day. The second consecutive
 * eligible night returns false (override), so a stuck activity refcount cannot
 * permanently suppress the release's core nightly mitigation. */
static bool measurement_veto_holds(int32_t today)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SYNC_MAINT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "measurement-veto latch open failed: %s — holding reboot",
                 esp_err_to_name(err));
        return true;
    }

    int32_t previous_day = 0;
    int32_t count = 0;
    esp_err_t day_err = nvs_get_i32(h, SYNC_MAINT_KEY_MEAS_VETO_DAY, &previous_day);
    esp_err_t count_err = nvs_get_i32(h, SYNC_MAINT_KEY_MEAS_VETO_COUNT, &count);
    if (day_err == ESP_ERR_NVS_NOT_FOUND) previous_day = 0;
    else if (day_err != ESP_OK) err = day_err;
    if (count_err == ESP_ERR_NVS_NOT_FOUND) count = 0;
    else if (count_err != ESP_OK) err = count_err;

    if (err == ESP_OK && previous_day != today) {
        int64_t previous_index = 0, today_index = 0;
        bool consecutive = calendar_day_index(previous_day, &previous_index) &&
                           calendar_day_index(today, &today_index) &&
                           today_index - previous_index == 1;
        count = consecutive && count > 0 ? count + 1 : 1;
        previous_day = today;
        err = nvs_set_i32(h, SYNC_MAINT_KEY_MEAS_VETO_DAY, previous_day);
        if (err == ESP_OK) err = nvs_set_i32(h, SYNC_MAINT_KEY_MEAS_VETO_COUNT, count);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "measurement-veto latch update failed: %s — holding reboot",
                 esp_err_to_name(err));
        return true;
    }
    if (count >= 2) {
        ESP_LOGW(TAG, "measurement active on %ld consecutive eligible nights — overriding veto",
                 (long)count);
        return false;
    }
    ESP_LOGW(TAG, "nightly reboot vetoed by active measurement (night %ld of 2)",
             (long)count);
    return true;
}

static bool nightly_reboot_due(uint32_t jitter_min, int32_t *day_out,
                               bool *clock_fallback_out)
{
    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    time_t now_s = time(NULL);
    bool clock_ok = now_s >= (time_t)SYNC_CLOCK_FLOOR_S;
    if (day_out != NULL) *day_out = 0;
    if (clock_fallback_out != NULL) *clock_fallback_out = !clock_ok;

    if (!clock_ok) {
        if (uptime_s <= SYNC_NIGHT_CLOCK_FALLBACK_S) return false;
        /* General maintenance is absolute: unlike the expiring OTA admission
         * hint, it proves flash/script/AMBIT work actually owns the worker. */
        if (maintenance_active() || ota_update_in_progress()) return false;
        if (device_commands_measurement_active() &&
            uptime_s <= SYNC_CLOCKLESS_MEAS_ESCALATE_S) return false;
        if (device_commands_measurement_active()) {
            ESP_LOGW(TAG, "clockless nightly fallback measurement veto exceeded two nights — overriding");
        }
        return true;
    }
    if (uptime_s <= SYNC_NIGHT_MIN_UPTIME_S) return false;

    int32_t offset_s = timezone_utc_offset_seconds((int64_t)now_s);
    time_t local_s = (time_t)((int64_t)now_s + offset_s);
    struct tm local_tm;
    if (gmtime_r(&local_s, &local_tm) == NULL) return false;

    uint32_t minute = (uint32_t)local_tm.tm_hour * 60U + (uint32_t)local_tm.tm_min;
    if (minute < SYNC_NIGHT_WINDOW_START_MIN + jitter_min ||
        minute >= SYNC_NIGHT_WINDOW_END_MIN) {
        return false;
    }

    int32_t today = (local_tm.tm_year + 1900) * 10000 +
                    (local_tm.tm_mon + 1) * 100 + local_tm.tm_mday;
    int32_t last_day = 0;
    esp_err_t err = maint_read_i32(SYNC_MAINT_KEY_REBOOT_DAY, &last_day);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND &&
        err != ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGE(TAG, "nightly reboot day latch read failed: %s — holding reboot",
                 esp_err_to_name(err));
        return false;
    }
    if (last_day == today) return false;
    if (day_out != NULL) *day_out = today;

    /* Apply dynamic vetoes only after all cheap eligibility checks. Maintenance
     * ownership stays absolute; the OTA admission hint self-expires after 30 m.
     * Measurement gets two eligible nights before its veto is overridden. */
    if (maintenance_active() || ota_update_in_progress()) return false;
    if (device_commands_measurement_active() && measurement_veto_holds(today)) return false;
    return true;
}

static void store_heartbeat(TickType_t now_tick, TickType_t hb_ticks,
                            TickType_t *last_hb)
{
    if (s_heartbeat_s == 0 || (now_tick - *last_hb) < hb_ticks) return;

    cmd_result_t hr = cmd_store_status_event();
    if (hr.status == ESP_OK) {
        ESP_LOGI(TAG, "%s", hr.message);
    } else if (hr.status != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "heartbeat: %s", hr.message);
    }
    ESP_LOGI(TAG, "watchdog stack high-water=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    *last_hb = now_tick;
}

/* Core decision, shared by the watchdog task and the status query. Fills the
 * out-params (any may be NULL) and returns true when a reboot is warranted:
 * on external power, the clock is valid, events are pending, AND no PUBACK has
 * landed within timeout_ms.
 *
 * Deliberately checks the POWER gate only — NOT sync_runner_is_allowed(), which
 * still includes short raw-sensor publish holds. This independence was prompted
 * by field data (2026-07-09, 5.6 h wedge): under the legacy full gate, a 9 s
 * measurement in a 10 s schedule kept publishing almost continuously blocked,
 * while the 60 s watchdog tick phase-locked to that cadence (60 = 6x10) and
 * sampled a closed gate every time. The new split removes that drain starvation,
 * but the liveness watchdog must remain blind to transient sensor holds: if no
 * PUBACK lands for an hour, the pipeline is stuck. A reboot mid-measurement loses
 * at most one round; the SD backlog survives. */
static bool sync_runner_wd_should_reboot(int64_t timeout_ms, bool *allowed,
                                         bool *clock_ok, int64_t *pending_out,
                                         int64_t *since_out)
{
    /* Last time the power gate was observed CLOSED (esp_timer ms). Starvation
     * must be measured from whichever came last: the last PUBACK or the last
     * closed-gate moment. Field night 2026-08-02 (first fleet night on 1.3.0):
     * battery devices close the gate at dusk while the schedule keeps queueing,
     * so ms_since_publish_ok spans the whole night; at dawn the gate reopens
     * with pending > 0 and "since" already hours past the timeout, and any
     * watchdog tick that lands before the first morning PUBACK reboots a
     * perfectly healthy device (3 devices latched "nopuback" that night).
     * Clamping to time-since-gate-open restarts the 1 h observation window at
     * reopen; a genuine wedge with the gate open still reboots on schedule. */
    static int64_t s_gate_blocked_ms;

    bool    a = device_commands_publish_power_ok();
    bool    c = time(NULL) >= (time_t)SYNC_CLOCK_FLOOR_S;
    int64_t pending = 0;
    (void)cmd_db_status(NULL, NULL, &pending, NULL);
    int64_t since = device_commands_ms_since_publish_ok();

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (!a) {
        s_gate_blocked_ms = now_ms;
    } else if (s_gate_blocked_ms > 0) {
        int64_t since_gate_open = now_ms - s_gate_blocked_ms;
        if (since_gate_open < since) since = since_gate_open;
    }

    if (allowed)     *allowed     = a;
    if (clock_ok)    *clock_ok    = c;
    if (pending_out) *pending_out = pending;
    if (since_out)   *since_out   = since;

    return a && c && pending > 0 && since > timeout_ms;
}

bool sync_runner_watchdog_status(bool *allowed, bool *clock_ok, int64_t *pending,
                                 int64_t *since_ms, int64_t *timeout_ms)
{
    if (timeout_ms) *timeout_ms = SYNC_WD_TIMEOUT_MS;
    return sync_runner_wd_should_reboot(SYNC_WD_TIMEOUT_MS, allowed, clock_ok,
                                        pending, since_ms);
}

void sync_runner_watchdog_test(void)
{
    s_wd_test = true;
    if (s_wd_task != NULL) xTaskNotifyGive(s_wd_task);   /* evaluate now */
}

static void sync_runner_wd_task(void *arg)
{
    (void)arg;

    const TickType_t wd_tick_ticks = pdMS_TO_TICKS(SYNC_WD_TICK_MS);
    const TickType_t hwm_ticks = pdMS_TO_TICKS(SYNC_TIME_HWM_INTERVAL_MS);
    const TickType_t hb_ticks = pdMS_TO_TICKS(s_heartbeat_s * 1000U);
    TickType_t now_tick = xTaskGetTickCount();
    /* Backdate both clocks so policy checks start immediately and the first
     * post-boot-complete heartbeat remains a boot marker. */
    TickType_t last_periodic = now_tick - wd_tick_ticks;
    TickType_t last_hb = now_tick - hb_ticks;
    TickType_t last_hwm = now_tick - hwm_ticks;
    uint32_t mem_low_samples = 0;
    uint32_t jitter_min = nightly_jitter_minutes();
    ESP_LOGI(TAG, "self-healing watchdog ready (nightly jitter=%u min)",
             (unsigned)jitter_min);

    while (1) {
        bool    test    = s_wd_test;
        s_wd_test       = false;
        now_tick = xTaskGetTickCount();
        bool periodic = (now_tick - last_periodic) >= wd_tick_ticks;

        /* Keep the backdated boot-marker semantics, but do not create STATUS
         * until app_main has finished AMBIT boot flash + Lua startup. The boot
         * completion notification wakes this task, so the marker is immediate. */
        if (s_boot_complete) store_heartbeat(now_tick, hb_ticks, &last_hb);

        if (periodic) {
            last_periodic = now_tick;

            /* The same independent task that owns liveness policy also owns
             * this low-rate durability tick. Backdating above creates a floor
             * on the first valid minute of a new installation; after that only
             * a strictly higher u32 epoch commits, once per hour. Pre-2024
             * clocks retry next minute without touching NVS. */
            if ((now_tick - last_hwm) >= hwm_ticks &&
                clock_trust_refresh_hwm() == ESP_OK) {
                last_hwm = now_tick;
            }

            int32_t reboot_day = 0;
            bool clock_fallback = false;
            if (nightly_reboot_due(jitter_min, &reboot_day, &clock_fallback)) {
                (void)self_reboot(SYNC_WD_REASON_NIGHTLY,
                    clock_fallback ? "clock invalid and uptime exceeded 26 h"
                                   : "local maintenance window reached",
                    true, reboot_day, NULL, true);
            }

            int64_t uptime_s = esp_timer_get_time() / 1000000LL;
            uint32_t disconnects = device_commands_mqtt_error_disconnects(
                SYNC_CONN_ERROR_WINDOW_S);
            int64_t conn_pending = 0, publish_stale_ms = 0;
            bool conn_starved = sync_runner_wd_should_reboot(
                SYNC_CONN_PUBLISH_STALE_MS, NULL, NULL,
                &conn_pending, &publish_stale_ms);
            time_t now_s = time(NULL);
            if (disconnects >= SYNC_CONN_ERROR_THRESHOLD &&
                conn_starved &&
                uptime_s > SYNC_CONN_MIN_UPTIME_S &&
                !maintenance_active() &&
                watchdog_guard_allows(SYNC_MAINT_KEY_CONN_REBOOT, "connection",
                                      SYNC_CONN_REBOOT_GUARD_S, now_s, uptime_s)) {
                char detail[192];
                snprintf(detail, sizeof detail,
                         "%u MQTT error-disconnects in rolling %u s; no PUBACK=%lld ms; pending=%lld; uptime=%lld s",
                         (unsigned)disconnects, (unsigned)SYNC_CONN_ERROR_WINDOW_S,
                         (long long)publish_stale_ms, (long long)conn_pending,
                         (long long)uptime_s);
                (void)self_reboot(SYNC_WD_REASON_CONN, detail, false, 0,
                                  SYNC_MAINT_KEY_CONN_REBOOT, true);
            }

            /* OTA legitimately consumes internal DRAM and is unsafe to interrupt
             * while writing flash. Drop the streak during it; sampling resumes on
             * the first quiet minute after the job finishes. */
            if (ota_update_in_progress()) {
                mem_low_samples = 0;
            } else {
                size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                mem_low_samples = largest < SYNC_MEM_LARGEST_MIN_BYTES
                    ? mem_low_samples + 1U : 0U;
                if (mem_low_samples >= SYNC_MEM_LOW_SAMPLES &&
                    uptime_s > SYNC_MEM_MIN_UPTIME_S &&
                    !maintenance_active() &&
                    watchdog_guard_allows(SYNC_MAINT_KEY_MEM_REBOOT, "memory",
                                          SYNC_MEM_REBOOT_GUARD_S, now_s, uptime_s)) {
                    char detail[128];
                    snprintf(detail, sizeof detail,
                             "largest internal block=%u B for %u consecutive samples",
                             (unsigned)largest, (unsigned)mem_low_samples);
                    (void)self_reboot(SYNC_WD_REASON_MEM, detail, false, 0,
                                      SYNC_MAINT_KEY_MEM_REBOOT, true);
                }
            }
        }

        /* Preserve the pre-existing no-PUBACK liveness guard with a latch that
         * is independent of connection churn. Test mode bypasses timeout/uptime/
         * guard delay and deliberately writes no production NVS latch. */
        if (periodic || test) {
            int64_t timeout = test ? -1 : SYNC_WD_TIMEOUT_MS;
            int64_t pending = 0, since = 0;
            int64_t uptime_s = esp_timer_get_time() / 1000000LL;
            if (!maintenance_active() &&
                sync_runner_wd_should_reboot(timeout, NULL, NULL, &pending, &since) &&
                (test || (uptime_s > SYNC_PUBACK_MIN_UPTIME_S &&
                          watchdog_guard_allows(SYNC_MAINT_KEY_PUBACK_REBOOT, "no-PUBACK",
                                                SYNC_PUBACK_REBOOT_GUARD_S,
                                                time(NULL), uptime_s)))) {
                char detail[192];
                snprintf(detail, sizeof detail,
                         "no successful publish for %lld ms (%lld pending, power+clock OK)%s",
                         (long long)since, (long long)pending, test ? " [TEST]" : "");
                (void)self_reboot(SYNC_WD_REASON_NOPUBACK, detail, false, 0,
                                  SYNC_MAINT_KEY_PUBACK_REBOOT, !test);
            }
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYNC_WD_TICK_MS));
    }
}

static void sync_runner_task(void *arg)
{
    (void)arg;

    /* Hold the first pass until app_main finishes the startup sequence, then
     * stagger past the MQTT TLS handshake that boot-complete just unlocked. */
    for (uint32_t waited = 0;
         !s_boot_complete && waited < SYNC_RUNNER_BOOT_WAIT_MAX_MS;
         waited += SYNC_RUNNER_BOOT_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(SYNC_RUNNER_BOOT_POLL_MS));
    }
    if (!s_boot_complete) {
        ESP_LOGW(TAG, "boot-complete signal never arrived — draining anyway");
    }
    vTaskDelay(pdMS_TO_TICKS(SYNC_RUNNER_START_DELAY_MS));

    bool clock_warned         = false;

    while (1) {
        /* Sleep until something stores an event / a measurement burst ends
         * (sync_runner_notify), or the fallback timer fires. pdTRUE clears the
         * notification count on take, so a burst of N stores collapses into one
         * drain pass. The CPU can idle in between — no fixed-rate polling. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYNC_RUNNER_FALLBACK_MS));

        /* Process completions before clock/power/sensor gates. Those gates stop
         * fresh network work; they must not leave an acknowledged or disconnected
         * event latched in persistence. on_publish_ack enqueues before notifying,
         * so a drain that previously stalled always observes the completion here. */
        esp_err_t ack_err = cmd_process_pending_acks();
        if (ack_err != ESP_OK) {
            ESP_LOGW(TAG, "deferred ack processing blocked (%s)", esp_err_to_name(ack_err));
            continue;
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
            ESP_LOGD(TAG, "sync gate closed (raw sensor transaction or on battery) — deferring");
        }
    }
}

esp_err_t sync_runner_start(uint32_t heartbeat_s)
{
    if (s_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_heartbeat_s = heartbeat_s;

    s_wd_armed = false;
    esp_err_t epoch_err = maint_begin_boot_epoch();
    if (epoch_err != ESP_OK) {
        /* Publishing remains useful, but self-reboots fail closed because their
         * reason generation cannot be made durable safely. */
        ESP_LOGE(TAG, "boot epoch unavailable (%s) — self-reboots will be blocked",
                 esp_err_to_name(epoch_err));
    }

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

    /* Connectivity/liveness watchdog — separate task so it survives a wedged
     * drain. Non-fatal if it can't start (the drain still runs). */
    if (xTaskCreate(sync_runner_wd_task, SYNC_WD_TASK_NAME, SYNC_WD_TASK_STACK,
                    NULL, SYNC_WD_TASK_PRIO, &s_wd_task) != pdPASS) {
        s_wd_task = NULL;
        ESP_LOGW(TAG, "watchdog task failed to start — STATUS heartbeat and nightly/"
                      "connection/memory/PUBACK self-healing disabled");
    } else {
        s_wd_armed = (epoch_err == ESP_OK);
    }

    ESP_LOGI(TAG, "background sync started (wake-on-store, fallback=%u ms, heartbeat=%u s, "
                  "watchdog=%llu ms)",
             (unsigned)SYNC_RUNNER_FALLBACK_MS, (unsigned)heartbeat_s,
             (unsigned long long)SYNC_WD_TIMEOUT_MS);
    return ESP_OK;
}
