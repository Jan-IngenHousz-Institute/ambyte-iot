#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "CLI.h"
#include "ambyte_mqtt_client.h"
#include "ambyte_status.h"
#include "certs.h"
#include "device_config.h"
#include "bme280.h"
#include "command_router.h"
#include "ota_update.h"
#include "ambit_ota.h"
#include "ambit_flash.h"
#include "script_update.h"
#include "device_commands.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "i2c_bus.h"
#include "lua_runner.h"
#include "mp2731.h"
#include "nvs_flash.h"
#include "pcf2131tfy_rtc_api.h"
#include "sd_card.h"
#include "sd_logger.h"
#include "event_log.h"
#include "sync_runner.h"
#include "uart_sensors.h"
#include "wifi_manager.h"

#define APP_TAG "APP_MAIN"

/* Human-visible build marker, logged loudly at boot. For an OTA HW test, bump
 * this in the image you publish (e.g. "ota-test-1") so the new image is
 * unmistakable in the boot log vs. the one flashed over USB. Overridable from
 * platformio.ini build_flags as -DAMBYTE_FW_TAG=... if you prefer. */
#ifndef AMBYTE_FW_TAG
#define AMBYTE_FW_TAG "dev"
#endif

/* Phase-2 DFS frequency window (MHz) — THE knob: change these two values, then
 * rebuild + flash. (PlatformIO build_flags do NOT reach ESP-IDF component sources
 * here — verified the -D never lands on this file's compile — so set it HERE, not
 * in platformio.ini. A Kconfig option is the alternative if a sdkconfig knob is
 * wanted.) Valid ESP32-S3 freqs: min in {40, 80}, max = 160 (max=240 also needs
 * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240). min=40 is HW-validated (1 h soak clean);
 * light_sleep stays off. */
#ifndef AMBYTE_PM_MIN_FREQ_MHZ
#define AMBYTE_PM_MIN_FREQ_MHZ 40
#endif
#ifndef AMBYTE_PM_MAX_FREQ_MHZ
#define AMBYTE_PM_MAX_FREQ_MHZ 160
#endif

/* Re-sync the ESP system clock from the RTC at this cadence (drift correction +
 * recovery if the RTC is set/validated after boot). */
#define APP_RTC_SYNC_INTERVAL_S 3600U

/* Replace the first occurrence of `token` in NUL-terminated `buf` (capacity
 * `cap`) with `repl`, in place. No-op if the token is absent or the result
 * wouldn't fit. Used to expand a {MAC} placeholder in the provisioned MQTT
 * client-id / topic-root so every board gets a unique, MAC-derived identity
 * (avoids the AWS IoT duplicate-client-id disconnect loop when several boards
 * share one .env / certificate). */
static void subst_token(char *buf, size_t cap, const char *token, const char *repl)
{
    char *at = strstr(buf, token);
    if (at == NULL) {
        return;
    }
    size_t tok_len = strlen(token);
    size_t rep_len = strlen(repl);
    size_t tail    = strlen(at + tok_len);            /* chars after the token (excl. NUL) */
    if ((size_t)(at - buf) + rep_len + tail + 1 > cap) {
        return;                                       /* would overflow — leave as-is */
    }
    memmove(at + rep_len, at + tok_len, tail + 1);    /* shift tail, incl. NUL */
    memcpy(at, repl, rep_len);
}

static const i2c_bus_config_t s_i2c_bus_cfg = {
    .port = I2C_BUS_DEFAULT_PORT,
    .sda_gpio = I2C_BUS_DEFAULT_SDA_GPIO,
    .scl_gpio = I2C_BUS_DEFAULT_SCL_GPIO,
    .clock_speed_hz = I2C_BUS_DEFAULT_SPEED_HZ,
};

static esp_err_t app_init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }

    return err;
}

/* ── Wi-Fi init + start (provisioning/connect logic in app_main) ─── */

static esp_err_t app_start_wifi(void)
{
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(APP_TAG, "Wi-Fi manager initialized");

    err = wifi_manager_start();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(APP_TAG, "Wi-Fi station started");
    return ESP_OK;
}

/* bool(void) adapter over wifi_manager_is_provisioned(bool*) for the
 * status-LED blinker probe table. */
static bool app_wifi_provisioned(void)
{
    bool provisioned = false;
    (void)wifi_manager_is_provisioned(&provisioned);
    return provisioned;
}

/* Pause/resume the Lua measurement task for the maintenance workers (self-OTA,
 * AMBIT OTA/probe/flash, script update) — stopping it frees its heap (8 KB AMBIT
 * buffer + transient tables) and the shared UART. REFCOUNTED: the workers run on
 * independent tasks, so overlapping suspend windows must not let one worker's
 * resume restart Lua inside another's quiesced window. */
static portMUX_TYPE s_workload_mux    = portMUX_INITIALIZER_UNLOCKED;
static int          s_workload_susp_n = 0;

static void app_workload_suspend(void)
{
    taskENTER_CRITICAL(&s_workload_mux);
    bool first = (s_workload_susp_n++ == 0);
    taskEXIT_CRITICAL(&s_workload_mux);
    if (first && lua_runner_stop(5000) != ESP_OK) {
        ESP_LOGW(APP_TAG, "Lua task did not stop before the maintenance op — "
                          "UART may be busy / heap may fragment");
    }
}

static void app_workload_resume(void)
{
    taskENTER_CRITICAL(&s_workload_mux);
    bool last = (s_workload_susp_n > 0 && --s_workload_susp_n == 0);
    taskEXIT_CRITICAL(&s_workload_mux);
    if (!last) return;

    /* Reloads /sdcard/main.lua. If the preceding stop TIMED OUT (script was stuck
     * in a long C call), the old task is still unwinding and start returns
     * INVALID_STATE — without a retry the measurement loop would silently stay
     * dead until a manual `lua start`/reboot. Retry until the old task exits. */
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 20; i++) {
        err = lua_runner_start();
        if (err != ESP_ERR_INVALID_STATE) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != ESP_OK) {
        ESP_LOGW(APP_TAG, "Lua restart after maintenance op: %s", esp_err_to_name(err));
    }
}

/* Global maintenance lock (Item D). The three maintenance workers — self-OTA
 * (ota_update), AMBIT OTA/flash/probe/versions (ambit_ota), and script update/
 * exec (script_update) — run on INDEPENDENT tasks with no shared "maintenance in
 * progress" gate. Two close-spaced commands of different types (or a QoS1
 * redelivery of one) could otherwise overlap: one worker resuming MQTT while
 * another still holds an HTTPS/TLS session → two TLS sessions on this no-PSRAM
 * board → OOM/crash — the exact thing the per-op quiesce exists to prevent; or one
 * worker's esp_restart firing while another is mid main.lua rename → no main.lua.
 * A single flag, taken at each worker's op entry and released on completion, makes
 * the three mutually exclusive; a second concurrent op of any type is rejected as
 * "busy"/"dropped". */
static portMUX_TYPE s_maint_mux  = portMUX_INITIALIZER_UNLOCKED;
static bool         s_maint_busy = false;

static bool app_maintenance_begin(void)
{
    bool acquired;
    taskENTER_CRITICAL(&s_maint_mux);
    acquired = !s_maint_busy;
    if (acquired) s_maint_busy = true;
    taskEXIT_CRITICAL(&s_maint_mux);
    return acquired;
}

static void app_maintenance_end(void)
{
    taskENTER_CRITICAL(&s_maint_mux);
    s_maint_busy = false;
    taskEXIT_CRITICAL(&s_maint_mux);
}

/* Refcounted MQTT suspend/resume for the maintenance workers (belt-and-suspenders
 * to the maintenance lock above). Raw mqtt_client_stop/start are idempotent but
 * NOT refcounted — this wrapper restarts MQTT only when the LAST holder resumes,
 * so an overlapping suspend window can never let one resume reopen MQTT (and its
 * TLS heap) inside another's quiesced download. Mirrors app_workload_suspend/
 * resume. (Wi-Fi lifecycle events still call mqtt_client_stop/start directly; both
 * are guarded by the client's own started-flag, so mixing is safe.) */
static portMUX_TYPE s_comms_mux    = portMUX_INITIALIZER_UNLOCKED;
static int          s_comms_susp_n = 0;

static void app_comms_suspend(void)
{
    taskENTER_CRITICAL(&s_comms_mux);
    bool first = (s_comms_susp_n++ == 0);
    taskEXIT_CRITICAL(&s_comms_mux);
    if (first) mqtt_client_stop();
}

static void app_comms_resume(void)
{
    taskENTER_CRITICAL(&s_comms_mux);
    bool last = (s_comms_susp_n > 0 && --s_comms_susp_n == 0);
    taskEXIT_CRITICAL(&s_comms_mux);
    if (last) mqtt_client_start();
}

/* Boot-complete latch: Wi-Fi connects asynchronously, so an IP can land while
 * app_main is still initializing — and starting the MQTT TLS handshake (plus,
 * 10 s later, the backlog drain) at that moment starved the rest of the boot;
 * the console REPL sometimes never came up. on_got_ip therefore PARKS the MQTT
 * start until app_open_boot_gate() runs at the end of the startup sequence
 * (CLI, AMBIT firmware sync, Lua). Mux-guarded: on_got_ip runs on the
 * esp_event task, concurrent with app_main. */
static portMUX_TYPE s_boot_mux      = portMUX_INITIALIZER_UNLOCKED;
static bool         s_boot_complete = false;
static bool         s_mqtt_deferred = false;

static bool app_boot_is_complete(void)
{
    taskENTER_CRITICAL(&s_boot_mux);
    bool done = s_boot_complete;
    taskEXIT_CRITICAL(&s_boot_mux);
    return done;
}

/* Open the boot gate: deferred MQTT + the upload drain may start. MUST run on
 * EVERY app_main exit path that is reachable after the Wi-Fi connect kick-off —
 * an early `return` that skips it parks MQTT forever (on_got_ip defers while
 * the latch is closed) and the device never phones home. Idempotent. */
static void app_open_boot_gate(void)
{
    taskENTER_CRITICAL(&s_boot_mux);
    s_boot_complete = true;
    bool start_mqtt = s_mqtt_deferred;
    s_mqtt_deferred = false;
    taskEXIT_CRITICAL(&s_boot_mux);
    /* Skip the parked start if Wi-Fi dropped while boot was finishing — the
     * next GOT_IP starts MQTT directly now that the latch is open. */
    if (start_mqtt && wifi_manager_is_connected()) {
        ESP_LOGI(APP_TAG, "boot complete — starting deferred MQTT");
        mqtt_client_start();
    }
    sync_runner_boot_complete();
}

static esp_err_t app_init_i2c_and_sensors(void)
{
    esp_err_t err = i2c_bus_init(&s_i2c_bus_cfg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(APP_TAG, "I2C bus initialized");

    err = pcf2131tfy_rtc_init();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "RTC init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(APP_TAG, "RTC initialized");
    }

    /* The provisioning NVS image embeds its build time (`flash_time`, see
     * tools/build_nvs_image.py). Used below as a clock bootstrap: applied only
     * when the clock is invalid or BEHIND it, so a correct RTC is never moved
     * (a running clock always reads ahead of the image build time). Accuracy =
     * the build→boot delay — minutes at worst in the normal flash workflow. */
    uint32_t flash_time = 0;
    if (device_config_get_flash_time(&flash_time) != ESP_OK) {
        flash_time = 0;
    }

    if (pcf2131tfy_rtc_is_ready()) {
        /* Push the RTC into the ESP-IDF system clock so gettimeofday / time(NULL)
         * return real UTC (every measurement startTicks + MQTT timestamp depends
         * on this). A never-set RTC reports its time as invalid (OSF) and is
         * skipped. */
        bool clock_ok = (pcf2131tfy_rtc_sync_system_clock() == ESP_OK);
        if (clock_ok) {
            ESP_LOGI(APP_TAG, "system clock synced from RTC");
        }

        if (flash_time != 0 && (!clock_ok || time(NULL) < (time_t)flash_time)) {
            struct tm tm_utc;
            time_t ft = (time_t)flash_time;
            gmtime_r(&ft, &tm_utc);
            /* Writing the RTC clears the oscillator-stop flag and re-syncs the
             * system clock — this is how a factory-fresh RTC comes online. */
            if (pcf2131tfy_rtc_set_time(&tm_utc) == ESP_OK) {
                ESP_LOGW(APP_TAG, "RTC %s — set from flash time %lu "
                                  "(accurate to the build->boot delay)",
                         clock_ok ? "behind the flash time" : "invalid",
                         (unsigned long)flash_time);
            } else {
                ESP_LOGW(APP_TAG, "RTC bootstrap from flash time failed — "
                                  "set the clock with `rtc set <epoch>`");
            }
        } else if (!clock_ok) {
            ESP_LOGW(APP_TAG, "RTC time invalid/unreadable — system clock not set; "
                              "set the RTC and it will auto-sync (no reboot needed)");
        }
        /* Re-sync periodically: corrects drift over long uptimes and recovers the
         * clock without a reboot if the RTC is set/validated after boot. */
        pcf2131tfy_rtc_start_periodic_sync(APP_RTC_SYNC_INTERVAL_S);
    } else if (flash_time != 0 && time(NULL) < (time_t)flash_time) {
        /* No RTC at all (dev board): seed the system clock from the flash time
         * so timestamps beat 1970. Not battery-backed — resets every reboot. */
        struct timeval tv = { .tv_sec = (time_t)flash_time };
        settimeofday(&tv, NULL);
        ESP_LOGW(APP_TAG, "no RTC — system clock seeded from flash time %lu "
                          "(volatile; resets on reboot)", (unsigned long)flash_time);
    }

    err = bme280_init(BME280_I2C_ADDR_SECONDARY);
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "BME280 init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(APP_TAG, "BME280 initialized");
    }

    /* MP2731 charger — provides battery/input power telemetry. Absent on dev
     * boards without the charger; the read path degrades gracefully. */
    if (mp2731_init() == ESP_OK) {
        ESP_LOGI(APP_TAG, "MP2731 charger initialized");
    }

    return ESP_OK;
}

static esp_err_t app_init_sdcard(void)
{
    esp_err_t err = sdcard_init_default();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "SD card init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(APP_TAG, "SD card mount failed, retrying in 500ms...");
        vTaskDelay(pdMS_TO_TICKS(500));
        err = sdcard_mount();
        if (err != ESP_OK) {
            ESP_LOGW(APP_TAG, "SD card mount retry failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(APP_TAG, "SD card mounted");
    return ESP_OK;
}

/* Hot-plug callback: fired by the sd_monitor task on every mount-state
 * transition. Drives the persistence layer AND the Lua runner so the script
 * is paused while the card is out and re-launched fresh when it returns —
 * which is the cleanest way to avoid running measurements against a closed
 * DB and to pick up any edits to /sdcard/main.lua on reinsert. */
static void app_on_sd_state_change(bool mounted)
{
    if (mounted) {
        /* DB first (the script will hit cmd_store_event almost immediately). */
        event_log_on_sd_restored();
        /* During boot (e.g. an SD bounce while the AMBIT firmware sync is
         * mid-flash) leave Lua to the boot sequence — starting it here would
         * break ambit_flash_boot_sync's pre-Lua exclusive-UART assumption. */
        if (!app_boot_is_complete()) {
            ESP_LOGW(APP_TAG, "SD restored during boot — Lua start left to the boot sequence");
            return;
        }
        esp_err_t err = lua_runner_start();
        if (err == ESP_OK) {
            ESP_LOGI(APP_TAG, "Lua runner restarted (SD inserted)");
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(APP_TAG, "Lua runner restart failed: %s", esp_err_to_name(err));
        }
    } else {
        /* Stop the script first so no in-flight ambit.run tries to write into
         * the DB while we're closing it. 5 s is enough for the script to
         * unwind from a sleep / short read; longer UART reads will finish in
         * the background and the task will exit on its own. */
        lua_runner_stop(5000);
        event_log_on_sd_lost();
    }
}

/* Pre-reboot power-safety hook (Item B). Every esp_restart() in the tree (OTA,
 * script_update, connectivity watchdog, CLI reboot, Wi-Fi clear) previously fired
 * with no fsync/unmount, and FATFS is not power-safe — a torn FAT/dir-entry write
 * could orphan clusters or, rarely, leave an unmountable card, which would stop
 * the measurement loop entirely (main.lua lives on the SD). Registering ONE
 * shutdown handler here (esp_register_shutdown_handler runs it before every
 * esp_restart) drains + closes the buffered SD writers and cleanly unmounts, so
 * ALL reboot paths benefit without touching each call site. Runs with the
 * scheduler still active, so the mutex/task handshakes inside are safe. */
static void app_prepare_reboot(void)
{
    event_log_prepare_shutdown();   /* flush + fsync + close the events tail */
    sd_logger_prepare_shutdown();   /* drain the log ring + close the log file */
    (void)sdcard_unmount();         /* finalize FATFS metadata (f_mount(NULL)) */
}

static esp_err_t app_init_littlefs(void)
{
    esp_vfs_littlefs_conf_t lfs_conf = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .grow_on_mount = true,
    };

    esp_err_t err = esp_vfs_littlefs_register(&lfs_conf);
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(APP_TAG, "LittleFS mounted");
    return ESP_OK;
}

static void app_start_lua_runner(bool sd_available)
{
    /* Lua script lives on the SD card (/sdcard/main.lua). Without SD the
     * loader would fail with a confusing "file not found"; skip cleanly
     * instead and surface the real reason in one line. */
    if (!sd_available) {
        ESP_LOGW(APP_TAG, "Lua runner not started: SD card not mounted");
        return;
    }

    const esp_err_t err = lua_runner_start();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "Lua runner failed to start: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(APP_TAG, "Lua runner started");
}

static void app_start_cli(void)
{
    const esp_err_t err = cli_start();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "CLI start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(APP_TAG, "CLI started");
}

/* ── Wi-Fi → MQTT lifecycle handlers ─────────────────────────────── */

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    taskENTER_CRITICAL(&s_boot_mux);
    bool defer = !s_boot_complete;
    if (defer) {
        s_mqtt_deferred = true;
    }
    taskEXIT_CRITICAL(&s_boot_mux);
    if (defer) {
        ESP_LOGI(APP_TAG, "IP acquired — MQTT start deferred until boot completes");
        return;
    }
    ESP_LOGI(APP_TAG, "IP acquired — starting MQTT");
    mqtt_client_start();
}

static void on_wifi_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    /* A drop before boot completes cancels any parked MQTT start — the next
     * GOT_IP re-parks (or starts) it as appropriate. */
    taskENTER_CRITICAL(&s_boot_mux);
    s_mqtt_deferred = false;
    taskEXIT_CRITICAL(&s_boot_mux);
    /* Only log + tear down if MQTT was actually running. Wi-Fi reconnect
     * attempts before we ever got an IP also fire this event, and there's
     * nothing to "stop" then — wifi_manager already logs the disconnect. */
    if (mqtt_client_is_running()) {
        ESP_LOGW(APP_TAG, "Wi-Fi disconnected — stopping MQTT");
        mqtt_client_stop();
        device_commands_on_mqtt_disconnect();
    }
}

void app_main(void)
{
    /* Capture WARN/ERROR logs to the SD card (INFO/DEBUG go to the console only).
     * Verbose continuous logging concurrent with the events DB corrupted the FAT
     * on a consumer card, so the file is now low-volume + idle-quiet by design. */
    if (sd_logger_init() != ESP_OK) {
        ESP_LOGW(APP_TAG, "SD logger failed to start");
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(APP_TAG, "app_main entered");
    ESP_LOGW(APP_TAG, "firmware build tag: %s", AMBYTE_FW_TAG);

    /* ── NVS ──────────────────────────────────────────────────────── */
    esp_err_t err = app_init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(APP_TAG, "NVS initialized");
    ESP_LOGI(APP_TAG, "Free heap after NVS: %lu", (unsigned long)esp_get_free_heap_size());

    /* ── Power management (Phase 2, DFS-only) ─────────────────────────
     * Dynamic frequency scaling 80<->160 MHz so the CPU coasts at 80 MHz during
     * the long idle gaps between measurements. light_sleep stays OFF for now
     * (that step needs tickless idle + a full peripheral-bracket audit). DFS is
     * safe here: the AMBIT UART is pinned to XTAL, I2C is DFS-aware, the SPI/SD
     * driver holds its own APB lock, and measurement windows hold a
     * NO_LIGHT_SLEEP pm_lock. Requires CONFIG_PM_ENABLE; logs (and is harmless)
     * if PM is somehow disabled. */
    {
        esp_pm_config_t pm_cfg = {
            .max_freq_mhz       = AMBYTE_PM_MAX_FREQ_MHZ,
            .min_freq_mhz       = AMBYTE_PM_MIN_FREQ_MHZ,
            .light_sleep_enable = false,
        };
        esp_err_t pmerr = esp_pm_configure(&pm_cfg);
        ESP_LOGI(APP_TAG, "power mgmt: DFS %d-%d MHz, light_sleep=off (%s)",
                 AMBYTE_PM_MIN_FREQ_MHZ, AMBYTE_PM_MAX_FREQ_MHZ,
                 esp_err_to_name(pmerr));
    }

    /* ── Status LED ─────────────────────────────────────────────── */
    err = ambyte_status_init();
    if (err != ESP_OK) {
        ESP_LOGW(APP_TAG, "Status LED init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(APP_TAG, "Status LED initialized");
    }

    /* ── Certs (NVS-backed) ──────────────────────────────────────── */
    err = certs_init();
    if (err != ESP_OK) {
        ESP_LOGW(APP_TAG, "certs_init failed: %s — TLS disabled", esp_err_to_name(err));
    }

    /* ── Runtime device config (NVS) ─────────────────────────────── */
    err = device_config_init();
    if (err != ESP_OK) {
        ESP_LOGW(APP_TAG, "device_config init failed: %s — using compile-time defaults",
                 esp_err_to_name(err));
    }

    /* ── Resolve config: NVS first, Kconfig fallback ─────────────── */
    static char mqtt_uri[256], mqtt_client_id[64], topic_root[256], device_id[64];
    if (device_config_get_mqtt_uri(mqtt_uri, sizeof(mqtt_uri)) != ESP_OK) {
        strncpy(mqtt_uri, CONFIG_AMBYTE_MQTT_URI, sizeof(mqtt_uri) - 1);
        mqtt_uri[sizeof(mqtt_uri) - 1] = '\0';
    }
    if (device_config_get_mqtt_client_id(mqtt_client_id, sizeof(mqtt_client_id)) != ESP_OK) {
        strncpy(mqtt_client_id, CONFIG_AMBYTE_MQTT_CLIENT_ID, sizeof(mqtt_client_id) - 1);
        mqtt_client_id[sizeof(mqtt_client_id) - 1] = '\0';
    }
    if (device_config_get_mqtt_topic_root(topic_root, sizeof(topic_root)) != ESP_OK) {
        strncpy(topic_root, CONFIG_AMBYTE_MQTT_TOPIC_ROOT, sizeof(topic_root) - 1);
        topic_root[sizeof(topic_root) - 1] = '\0';
    }

    /* Expand a {MAC} placeholder in the provisioned client-id / topic-root with
     * this board's Wi-Fi STA MAC, so boards sharing one .env get unique MQTT
     * identities (no AWS IoT duplicate-client-id disconnect loop). esp_read_mac
     * reads the efuse MAC directly — valid here even though Wi-Fi isn't started
     * yet (MQTT is configured before wifi_manager init). If neither value
     * contains {MAC}, this is a no-op and behaviour is unchanged.
     * NOTE: the resulting client-id/topic must be permitted by the device's AWS
     * IoT policy (which often pins them to the thing name) or the broker will
     * reject the connection. Colons match AMBYTE_DEVICE_ID style; switch the
     * format to colon-free hex here if AWS rejects them. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    subst_token(mqtt_client_id, sizeof(mqtt_client_id), "{MAC}", mac_str);
    subst_token(topic_root,     sizeof(topic_root),     "{MAC}", mac_str);
    ESP_LOGI(APP_TAG, "MQTT identity: client_id=%s", mqtt_client_id);
    if (device_config_get_device_id(device_id, sizeof(device_id)) != ESP_OK) {
        strncpy(device_id, CONFIG_AMBYTE_DEVICE_ID, sizeof(device_id) - 1);
        device_id[sizeof(device_id) - 1] = '\0';
    }
    static char protocol_id[32], device_name[64], device_version[16],
                device_firmware[16], firmware_version[16], timezone[48];
    if (device_config_get_protocol_id(protocol_id, sizeof(protocol_id)) != ESP_OK) {
        protocol_id[0] = '\0';
    }
    if (device_config_get_device_name(device_name, sizeof(device_name)) != ESP_OK) {
        device_name[0] = '\0';
    }
    /* Expand {MAC} in device_name too — same placeholder support as client-id/
     * topic-root above, so a shared .env (AMBYTE_DEVICE_NAME=AMBYTE_{MAC}) yields
     * a per-board name in the payload instead of the literal token. No-op when
     * device_name has no {MAC} (e.g. the AmbyteOnAir default). */
    subst_token(device_name, sizeof(device_name), "{MAC}", mac_str);
    if (device_config_get_device_version(device_version, sizeof(device_version)) != ESP_OK) {
        device_version[0] = '\0';
    }
    if (device_config_get_device_firmware(device_firmware, sizeof(device_firmware)) != ESP_OK) {
        device_firmware[0] = '\0';
    }
    if (device_config_get_firmware_version(firmware_version, sizeof(firmware_version)) != ESP_OK) {
        firmware_version[0] = '\0';
    }
    if (device_config_get_timezone(timezone, sizeof(timezone)) != ESP_OK) {
        timezone[0] = '\0';
    }

    /* Inbound command channel (Stage 2): full authorized topics from NVS config
     * (the AWS IoT policy grants Subscribe on cmd_topic, Publish on status_topic).
     * These are deployment-specific and independent of the telemetry topic_root —
     * they use the Thing name, not the MAC client-id. Fall back to a topic_root
     * derivation only if unprovisioned (older NVS). */
    static char command_topic[288], status_topic[288];
    if (device_config_get_command_topic(command_topic, sizeof(command_topic)) != ESP_OK) {
        snprintf(command_topic, sizeof(command_topic), "%s/cmd", topic_root);
    }
    /* One-time migration (2026-06-15): early units were provisioned with the reply
     * topic under experiment UUID a3b865d1, which the AWS IoT policy does NOT
     * authorize for publish — every command reply (pong/script_status/ota_status)
     * was rejected and dropped the device's MQTT connection. Repoint it to the
     * authorized 665b6b18 experiment with a /status leaf (policy grants
     * .../AMBYTE<MAC>/<seg>). Idempotent: fires only while the bad UUID is present.
     * Delivered via OTA, so field units self-heal without a reflash (OTA never
     * writes NVS); the corrected value is then read by the line below this boot. */
    {
        char cur_status[288];
        if (device_config_get_status_topic(cur_status, sizeof(cur_status)) == ESP_OK &&
            strstr(cur_status, "a3b865d1") != NULL) {
            esp_err_t merr = device_config_set_status_topic(
                "experiment/data_ingest/v1/665b6b18-3cfe-4d0a-85c7-3e84fa2f7834"
                "/multispeq/v1.0/AMBYTE_{MAC}/status");
            ESP_LOGW(APP_TAG,
                     "status-topic migration: a3b865d1 -> 665b6b18/.../AMBYTE_{MAC}/status (%s)",
                     esp_err_to_name(merr));
        }
    }
    if (device_config_get_status_topic(status_topic, sizeof(status_topic)) != ESP_OK) {
        snprintf(status_topic, sizeof(status_topic), "%s/status", topic_root);
    }
    /* Expand {MAC} here too — same placeholder support as client-id/topic-root,
     * so a shared .env can scope per-board command/status topics. No-op when the
     * provisioned topics use a fixed name (e.g. the Thing name). */
    subst_token(command_topic, sizeof(command_topic), "{MAC}", mac_str);
    subst_token(status_topic,  sizeof(status_topic),  "{MAC}", mac_str);

    /* ── MQTT Client ─────────────────────────────────────────────── */
    bool certs_ok = certs_are_provisioned();
    mqtt_client_config_t mqtt_cfg = {
        .broker_uri      = mqtt_uri,
        .client_id       = mqtt_client_id,
        .ca_cert_pem     = certs_ok ? certs_get_ca_cert()     : NULL,
        .device_cert_pem = certs_ok ? certs_get_device_cert() : NULL,
        .device_key_pem  = certs_ok ? certs_get_device_key()  : NULL,
        .command_topic   = command_topic,
    };
    err = mqtt_client_init(&mqtt_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(APP_TAG, "MQTT client init failed: %s — MQTT disabled", esp_err_to_name(err));
    }

    /* Route inbound commands (ping/ota_update/script_update) to the command router. */
    command_router_config_t cr_cfg = {
        .publish          = mqtt_client_get_publish_fn(),
        .status_topic     = status_topic,
        .device_id        = device_id,
        .firmware_version = firmware_version,
    };
    if (command_router_init(&cr_cfg) == ESP_OK) {
        mqtt_client_get_set_received_handler_fn()(command_router_get_received_fn(), NULL);
        ESP_LOGI(APP_TAG, "command router wired (cmd topic: %s)", command_topic);
    }

    /* MQTT-triggered self-OTA (docs/ota-update-plan.md Stage 3). The worker
     * suspends MQTT during the download (the board can't hold two TLS sessions),
     * and confirms a just-applied image on the next MQTT reconnect (else rolls
     * back). Triggered by command_router's ota_update dispatch. */
    ota_update_config_t ota_cfg = {
        .publish           = mqtt_client_get_publish_fn(),
        .is_connected      = mqtt_client_get_is_connected_fn(),
        .comms_suspend     = app_comms_suspend,
        .comms_resume      = app_comms_resume,
        .workload_suspend  = app_workload_suspend,
        .workload_resume   = app_workload_resume,
        .maintenance_begin = app_maintenance_begin,
        .maintenance_end   = app_maintenance_end,
        .status_topic      = status_topic,
        .device_id         = device_id,
    };
    if (ota_update_init(&ota_cfg) != ESP_OK) {
        ESP_LOGW(APP_TAG, "OTA worker not started");
    }

    /* Host-driven AMBIT (C3) firmware update over UART. CLI-triggered
     * (`ambit_ota <ch> <url>`): downloads the C3 image to SD, suspends Lua + MQTT,
     * streams it to the sensor over the binary UART link; the AMBIT verifies and
     * reboots into its spare slot. Same quiesce hooks as the self-OTA. */
    ambit_ota_config_t ambit_ota_cfg = {
        .workload_suspend  = app_workload_suspend,
        .workload_resume   = app_workload_resume,
        .comms_suspend     = app_comms_suspend,
        .comms_resume      = app_comms_resume,
        .maintenance_begin = app_maintenance_begin,
        .maintenance_end   = app_maintenance_end,
        .publish           = mqtt_client_get_publish_fn(),
        .is_connected      = mqtt_client_get_is_connected_fn(),
        .status_topic      = status_topic,
        .device_id         = device_id,
    };
    if (ambit_ota_init(&ambit_ota_cfg) != ESP_OK) {
        ESP_LOGW(APP_TAG, "AMBIT OTA worker not started");
    }

    /* Remote Lua control (Stage 4): MQTT script_update replaces /sdcard/main.lua
     * (syntax-checked, .bak kept) + restarts the runner; MQTT lua_exec runs a
     * snippet in an ephemeral state. Lazy worker — no steady-state heap cost. */
    script_update_config_t script_cfg = {
        .publish          = mqtt_client_get_publish_fn(),
        .is_connected     = mqtt_client_get_is_connected_fn(),
        .status_topic     = status_topic,
        .device_id        = device_id,
        /* url variant: stop Lua (defragment) AND stop MQTT (free its TLS heap)
         * around the HTTPS download so the download's TLS handshake gets a clean
         * contiguous heap — same quiesce hooks as OTA. MQTT is resumed before the
         * status reply. */
        .workload_suspend  = app_workload_suspend,
        .workload_resume   = app_workload_resume,
        .comms_suspend     = app_comms_suspend,
        .comms_resume      = app_comms_resume,
        .maintenance_begin = app_maintenance_begin,
        .maintenance_end   = app_maintenance_end,
    };
    if (script_update_init(&script_cfg) != ESP_OK) {
        ESP_LOGW(APP_TAG, "script_update worker not started");
    }

    /* ── Wi-Fi init + start ───────────────────────────────────────── */
    err = app_start_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "Wi-Fi startup failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(APP_TAG, "Free heap after WiFi: %lu", (unsigned long)esp_get_free_heap_size());

    /* ── Connect using NVS-stored credentials ─────────────────────── *
     * Provisioning is now delivered out-of-band by tools/build_nvs_image.py
     * flashing the NVS partition alongside the firmware. If the NVS hasn't
     * been pre-populated (or wifi_creds was not seeded), Wi-Fi simply fails
     * to connect and the device keeps running for CLI debugging. */
    bool provisioned = false;
    err = wifi_manager_is_provisioned(&provisioned);
    if (err != ESP_OK) {
        ESP_LOGW(APP_TAG, "Provisioning check failed: %s", esp_err_to_name(err));
    }
    if (!provisioned) {
        ambyte_status_set_rgb(20, 0, 0);  /* red = unprovisioned */
        ESP_LOGE(APP_TAG, "Device not provisioned — run tools/build_nvs_image.py and re-flash NVS");
    }

    /* ── Wi-Fi → MQTT lifecycle event handlers ────────────────────── */
    /* Registered BEFORE initiating the connect so a fast GOT_IP can't be missed:
     * on_got_ip starts MQTT, on_wifi_disconnect tears it down. The non-BT,
     * NVS-seeded provisioning path has no interactive flow these could disturb. */
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,         on_got_ip,          NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,  on_wifi_disconnect, NULL);

    /* Kick off Wi-Fi WITHOUT blocking the boot. A missing AP (reason=201,
     * NO_AP_FOUND) would otherwise stall here for the full connect timeout,
     * delaying everything below — sensors, SD, and the Lua measurement loop, none
     * of which depend on Wi-Fi. Publishing is power-gated and drains in the
     * background once on_got_ip fires; the connect + MQTT start complete
     * asynchronously via the events registered above. */
    err = wifi_manager_connect_stored_async();
    if (err == ESP_OK) {
        ESP_LOGI(APP_TAG, "Wi-Fi connect initiated (continues in background)");
    } else {
        ESP_LOGW(APP_TAG, "Wi-Fi connect could not start: %s", esp_err_to_name(err));
    }

    /* ── I2C + Sensors ────────────────────────────────────────────── */
    err = app_init_i2c_and_sensors();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "I2C startup failed: %s", esp_err_to_name(err));
        /* Aborting boot, but Wi-Fi is already connecting: open the gate so the
         * broken unit can still phone home over MQTT for remote diagnosis. */
        app_open_boot_gate();
        return;
    }

    /* ── UART Sensors (4 channels, Option D) ────────────────────── */
    bool uart_available = false;
    err = uart_sensors_init();
    if (err == ESP_OK) {
        uart_available = true;
        ESP_LOGI(APP_TAG, "UART sensors initialized");
        /* Auto-ping each channel at boot (up to 2 s per channel) */
        for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
            bool conn = false;
            uart_sensors_get_ping_fn()(ch, &conn);
            ESP_LOGI(APP_TAG, "  AMBIT%u: %s", ch + 1,
                     conn ? "connected" : "disconnected");
        }
    } else {
        ESP_LOGW(APP_TAG, "UART sensors init failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(APP_TAG, "Free heap after UART: %lu", (unsigned long)esp_get_free_heap_size());

    /* ── SD Card ──────────────────────────────────────────────────── */
    bool sd_available = false;
    err = app_init_sdcard();
    if (err == ESP_OK) {
        sd_available = true;
    } else {
        ESP_LOGW(APP_TAG, "SD card unavailable, buffering to LittleFS only");
    }

    /* ── LittleFS ─────────────────────────────────────────────────── */
    bool lfs_available = false;
    err = app_init_littlefs();
    if (err == ESP_OK) {
        lfs_available = true;
    } else {
        ESP_LOGE(APP_TAG, "LittleFS unavailable, persistence disabled");
    }

    /* ── Persistence (append-only event log) ──────────────────────── */
    bool persistence_available = false;
    if (lfs_available) {
        err = event_log_init();
        if (err == ESP_OK) {
            persistence_available = true;
            ESP_LOGI(APP_TAG, "Persistence layer ready");
        } else {
            ESP_LOGW(APP_TAG, "Persistence init failed: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(APP_TAG, "Free heap after persistence: %lu", (unsigned long)esp_get_free_heap_size());

    /* ── SD hot-plug monitor ──────────────────────────────────────── */
    if (sd_available) {
        esp_err_t mon_err = sdcard_start_monitor(2000, app_on_sd_state_change);
        if (mon_err != ESP_OK) {
            ESP_LOGW(APP_TAG, "SD monitor failed to start: %s", esp_err_to_name(mon_err));
        }
    }

    /* ── Pre-reboot SD power-safety hook (Item B) ─────────────────────
     * Registered once; esp_register_shutdown_handler invokes it before every
     * esp_restart(), so the event log + SD log are flushed and FATFS is cleanly
     * unmounted on all reboot paths. */
    if (sd_available) {
        if (esp_register_shutdown_handler(app_prepare_reboot) != ESP_OK) {
            ESP_LOGW(APP_TAG, "could not register SD pre-reboot flush handler");
        }
    }

    /* ── Hardware inventory ───────────────────────────────────────── */
    ESP_LOGI(APP_TAG, "BOOT: BME280=%s RTC=%s SD=%s LFS=%s DB=%s UART=%s",
             bme280_is_ready() ? "OK" : "ABSENT",
             pcf2131tfy_rtc_is_ready() ? "OK" : "ABSENT",
             sd_available ? "OK" : "ABSENT",
             lfs_available ? "OK" : "ABSENT",
             persistence_available ? "OK" : "ABSENT",
             uart_available ? "OK" : "ABSENT");

    /* ── Compose device_commands (DDD composition root) ───────────── */
    device_commands_config_t cmd_cfg = {
        .read_env               = bme280_get_sensor_read_fn(),
        .read_clock             = pcf2131tfy_rtc_get_clock_read_fn(),
        .read_power             = mp2731_is_ready() ? mp2731_get_power_read_fn() : NULL,
        .set_status             = ambyte_status_get_set_fn(),
        .sd_ready               = sd_available ? sdcard_is_mounted : NULL,
        .next_id            = persistence_available ? event_log_get_next_id_fn()            : NULL,
        .store_event        = persistence_available ? event_log_get_store_event_fn()        : NULL,
        .claim_next_event   = persistence_available ? event_log_get_claim_next_event_fn()   : NULL,
        .mark_event_synced  = persistence_available ? event_log_get_mark_event_synced_fn()  : NULL,
        .mark_event_pending = persistence_available ? event_log_get_mark_event_pending_fn() : NULL,
        .quarantine_event   = persistence_available ? event_log_get_quarantine_fn()         : NULL,
        .db_stats           = persistence_available ? event_log_get_db_stats_fn()           : NULL,
        .publish                = mqtt_client_get_publish_fn(),
        .message_is_connected   = mqtt_client_get_is_connected_fn(),
        .set_publish_ack_handler = mqtt_client_get_set_ack_handler_fn(),
        .set_disconnect_handler = mqtt_client_get_set_disconnect_handler_fn(),
        .topic_root             = topic_root,
        .device_id              = device_id,
        .protocol_id            = protocol_id,
        .device_name            = device_name,
        .device_version         = device_version,
        .device_firmware        = device_firmware,
        .timezone               = timezone,
        .uart_query             = uart_available ? uart_sensors_get_query_fn()       : NULL,
        .uart_ping              = uart_available ? uart_sensors_get_ping_fn()        : NULL,
        .uart_status            = uart_available ? uart_sensors_get_status_fn()      : NULL,
        .uart_text_query        = uart_available ? uart_sensors_get_text_query_fn()  : NULL,
        .uart_stream_query      = uart_available ? uart_sensors_get_stream_query_fn(): NULL,
        .request_gc             = lua_runner_request_gc,
    };
    device_commands_init(&cmd_cfg);

    /* ── CLI ──────────────────────────────────────────────────────────
     * Console first: everything below (sync runner, AMBIT firmware sync, Lua)
     * can take seconds to minutes, and the operator needs a live prompt while
     * it runs. Commands degrade gracefully for anything not started yet. */
    app_start_cli();

    /* ── Background MQTT sync + STATUS heartbeat ─────────────────────── */
    if (persistence_available) {
        uint32_t heartbeat_s = 300;                       /* default 5 min */
        (void)device_config_get_heartbeat_s(&heartbeat_s); /* NVS override */
        esp_err_t sr_err = sync_runner_start(heartbeat_s);
        if (sr_err != ESP_OK) {
            ESP_LOGW(APP_TAG, "sync_runner_start failed: %s", esp_err_to_name(sr_err));
        }
    }

    /* ── Field-status LED blinker (firmware-owned; Lua no longer drives the
     * LED). Probes are cheap reads of already-cached state. ─────────────── */
    {
        static const ambyte_blinker_config_t blink_cfg = {
            .sd_mounted        = sdcard_is_mounted,
            .wifi_connected    = wifi_manager_is_connected,
            .provisioned       = app_wifi_provisioned,
            .script_running    = lua_runner_is_running,
            .on_external_power = device_commands_publish_power_ok,
            .battery_mv        = device_commands_last_battery_mv,
        };
        if (ambyte_status_blinker_start(&blink_cfg) != ESP_OK) {
            ESP_LOGW(APP_TAG, "status-LED blinker not started");
        }
    }

    /* ── AMBIT firmware sync from SD ──────────────────────────────────
     * BEFORE Lua ever starts: the shared UART is still free (no quiesce dance)
     * and probing may hard-reset all four AMBITs. Bricked/blank units are
     * revived via the ROM bootloader; flashing is power-gated like MQTT
     * publishing. Detect-only `ambit_check` remains available from the CLI. */
    if (sd_available && uart_available) {
        (void)ambit_flash_boot_sync();
    }

    /* ── Start the measurement loop ───────────────────────────────── */
    app_start_lua_runner(sd_available);

    /* ── Boot complete: release MQTT + the upload drain ───────────────
     * Everything is initialized and Lua is running — the TLS handshake and
     * backlog drain can no longer compete with the startup sequence. */
    app_open_boot_gate();

    ESP_LOGI(APP_TAG, "Startup sequence complete, free heap: %lu, largest block: %u",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
