#include <stdbool.h>
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
#include "device_commands.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "i2c_bus.h"
#include "lua_runner.h"
#include "nvs_flash.h"
#include "pcf2131tfy_rtc_api.h"
#include "sd_card.h"
#include "sd_logger.h"
#include "sqlite_persistence.h"
#include "sync_runner.h"
#include "uart_sensors.h"
#include "wifi_manager.h"

#define APP_TAG "APP_MAIN"

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

    if (pcf2131tfy_rtc_is_ready()) {
        time_t rtc_now = 0;
        if (pcf2131tfy_rtc_get_time(&rtc_now) == ESP_OK) {
            /* Push the RTC value into the ESP-IDF system clock so subsequent
             * gettimeofday / time(NULL) calls return real UTC instead of
             * seconds-since-boot. Without this, every measurement timestamp
             * and every MQTT publish "timestamp" field comes out as 1970+uptime. */
            struct timeval tv = { .tv_sec = rtc_now, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(APP_TAG, "RTC time: %lld (system clock synced)", (long long)rtc_now);
        } else {
            ESP_LOGW(APP_TAG, "RTC time read failed");
        }
    }

    err = bme280_init(BME280_I2C_ADDR_SECONDARY);
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "BME280 init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(APP_TAG, "BME280 initialized");
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
        sqlite_persistence_on_sd_restored();
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
        sqlite_persistence_on_sd_lost();
    }
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
    ESP_LOGI(APP_TAG, "IP acquired — starting MQTT");
    mqtt_client_start();
}

static void on_wifi_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
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

    /* ── NVS ──────────────────────────────────────────────────────── */
    esp_err_t err = app_init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(APP_TAG, "NVS initialized");
    ESP_LOGI(APP_TAG, "Free heap after NVS: %lu", (unsigned long)esp_get_free_heap_size());

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
    if (device_config_get_device_id(device_id, sizeof(device_id)) != ESP_OK) {
        strncpy(device_id, CONFIG_AMBYTE_DEVICE_ID, sizeof(device_id) - 1);
        device_id[sizeof(device_id) - 1] = '\0';
    }
    static char protocol_id[32], device_name[64], device_version[16],
                device_firmware[16], firmware_version[16];
    if (device_config_get_protocol_id(protocol_id, sizeof(protocol_id)) != ESP_OK) {
        protocol_id[0] = '\0';
    }
    if (device_config_get_device_name(device_name, sizeof(device_name)) != ESP_OK) {
        device_name[0] = '\0';
    }
    if (device_config_get_device_version(device_version, sizeof(device_version)) != ESP_OK) {
        device_version[0] = '\0';
    }
    if (device_config_get_device_firmware(device_firmware, sizeof(device_firmware)) != ESP_OK) {
        device_firmware[0] = '\0';
    }
    if (device_config_get_firmware_version(firmware_version, sizeof(firmware_version)) != ESP_OK) {
        firmware_version[0] = '\0';
    }

    /* ── MQTT Client ─────────────────────────────────────────────── */
    bool certs_ok = certs_are_provisioned();
    mqtt_client_config_t mqtt_cfg = {
        .broker_uri      = mqtt_uri,
        .client_id       = mqtt_client_id,
        .ca_cert_pem     = certs_ok ? certs_get_ca_cert()     : NULL,
        .device_cert_pem = certs_ok ? certs_get_device_cert() : NULL,
        .device_key_pem  = certs_ok ? certs_get_device_key()  : NULL,
    };
    err = mqtt_client_init(&mqtt_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(APP_TAG, "MQTT client init failed: %s — MQTT disabled", esp_err_to_name(err));
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

    err = wifi_manager_connect_stored();
    if (err == ESP_OK) {
        ESP_LOGI(APP_TAG, "Wi-Fi connected");
    } else if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(APP_TAG, "Wi-Fi initial connect timed out; reconnect continues in background");
    } else {
        ESP_LOGW(APP_TAG, "Wi-Fi connect failed: %s", esp_err_to_name(err));
    }

    /* ── Wi-Fi → MQTT lifecycle event handlers ────────────────────── */
    /* Registered after the provisioning/connect block so they cannot fire
     * during provisioning and trigger an MQTT start mid-session. */
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,         on_got_ip,          NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,  on_wifi_disconnect, NULL);

    /* Start MQTT now if WiFi was already connected during the connect block */
    if (wifi_manager_is_connected()) {
        ESP_LOGI(APP_TAG, "WiFi already up — starting MQTT");
        mqtt_client_start();
    }

    /* ── I2C + Sensors ────────────────────────────────────────────── */
    err = app_init_i2c_and_sensors();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "I2C startup failed: %s", esp_err_to_name(err));
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

    /* ── Persistence (SQLite) ─────────────────────────────────────── */
    bool persistence_available = false;
    if (lfs_available) {
        err = sqlite_persistence_init();
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
        .set_status             = ambyte_status_get_set_fn(),
        .sd_ready               = sd_available ? sdcard_is_mounted : NULL,
        .next_id            = persistence_available ? sqlite_persistence_get_next_id_fn()            : NULL,
        .store_event        = persistence_available ? sqlite_persistence_get_store_event_fn()        : NULL,
        .claim_next_event   = persistence_available ? sqlite_persistence_get_claim_next_event_fn()   : NULL,
        .mark_event_synced  = persistence_available ? sqlite_persistence_get_mark_event_synced_fn()  : NULL,
        .mark_event_pending = persistence_available ? sqlite_persistence_get_mark_event_pending_fn() : NULL,
        .db_stats           = persistence_available ? sqlite_persistence_get_db_stats_fn()           : NULL,
        .publish                = mqtt_client_get_publish_fn(),
        .message_is_connected   = mqtt_client_get_is_connected_fn(),
        .set_publish_ack_handler = mqtt_client_get_set_ack_handler_fn(),
        .topic_root             = topic_root,
        .device_id              = device_id,
        .protocol_id            = protocol_id,
        .device_name            = device_name,
        .device_version         = device_version,
        .device_firmware        = device_firmware,
        .firmware_version       = firmware_version,
        .uart_query             = uart_available ? uart_sensors_get_query_fn()       : NULL,
        .uart_ping              = uart_available ? uart_sensors_get_ping_fn()        : NULL,
        .uart_status            = uart_available ? uart_sensors_get_status_fn()      : NULL,
        .uart_text_query        = uart_available ? uart_sensors_get_text_query_fn()  : NULL,
        .uart_stream_query      = uart_available ? uart_sensors_get_stream_query_fn(): NULL,
    };
    device_commands_init(&cmd_cfg);

    /* ── Background MQTT sync (publishes PENDING measurements every 10s) ── */
    if (persistence_available) {
        esp_err_t sr_err = sync_runner_start();
        if (sr_err != ESP_OK) {
            ESP_LOGW(APP_TAG, "sync_runner_start failed: %s", esp_err_to_name(sr_err));
        }
    }

    /* ── Start application tasks ──────────────────────────────────── */
    app_start_lua_runner(sd_available);
    app_start_cli();

    ESP_LOGI(APP_TAG, "Startup sequence complete, free heap: %lu",
             (unsigned long)esp_get_free_heap_size());
}
