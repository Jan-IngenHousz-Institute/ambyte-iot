#include "device_config.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "timezone.h"

#define TAG          "device_cfg"
#define NVS_NS       "device_cfg"

/* NVS key names — max 15 chars (NVS_KEY_NAME_MAX_SIZE = 16 incl. null) */
#define KEY_MQTT_URI        "mqtt_uri"
#define KEY_MQTT_CLIENT_ID  "mqtt_client_id"
#define KEY_MQTT_TOPIC_ROOT "mqtt_topic_root"
#define KEY_COMMAND_TOPIC   "cmd_topic"
#define KEY_STATUS_TOPIC    "status_topic"
#define KEY_DEVICE_ID       "device_id"
#define KEY_PROTOCOL_ID     "protocol_id"
#define KEY_DEVICE_NAME     "device_name"
#define KEY_DEVICE_VERSION  "device_ver"
#define KEY_DEVICE_FIRMWARE "device_firm"
#define KEY_FIRMWARE_VER    "firmware_ver"
#define KEY_TIMEZONE        "timezone"
#define KEY_FLASH_TIME      "flash_time"
#define KEY_HEARTBEAT_S     "heartbeat_s"

static nvs_handle_t s_handle    = 0;
static bool         s_initialized = false;

esp_err_t device_config_init(void)
{
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;

    /* One-time OTA-safe migration for early provisioning images that used AMT
     * as shorthand for Amsterdam. Do not rewrite unknown values: retain them in
     * NVS for diagnosis, while device_config_get_timezone() below fails closed
     * so they can never enter an MQTT envelope. */
    char stored[64];
    size_t stored_len = sizeof(stored);
    err = nvs_get_str(s_handle, KEY_TIMEZONE, stored, &stored_len);
    if (err == ESP_OK) {
        char canonical[48];
        esp_err_t tzerr = timezone_canonicalize(stored, canonical, sizeof(canonical));
        if (tzerr == ESP_OK && strcmp(stored, canonical) != 0) {
            tzerr = nvs_set_str(s_handle, KEY_TIMEZONE, canonical);
            if (tzerr == ESP_OK) tzerr = nvs_commit(s_handle);
            if (tzerr == ESP_OK) {
                ESP_LOGW(TAG, "timezone migrated '%s' -> '%s'", stored, canonical);
            } else {
                ESP_LOGE(TAG, "timezone migration failed: %s", esp_err_to_name(tzerr));
                return tzerr;
            }
        } else if (tzerr != ESP_OK) {
            ESP_LOGE(TAG, "invalid stored timezone '%s' — omitted from telemetry", stored);
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "timezone migration read failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "device_config initialised");
    return ESP_OK;
}

static esp_err_t cfg_get(const char *key, char *buf, size_t len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    size_t out_len = len;
    return nvs_get_str(s_handle, key, buf, &out_len);
}

static esp_err_t cfg_set(const char *key, const char *val)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t err = nvs_set_str(s_handle, key, val);
    if (err != ESP_OK) return err;
    return nvs_commit(s_handle);
}

esp_err_t device_config_get_mqtt_uri(char *buf, size_t len)
{
    return cfg_get(KEY_MQTT_URI, buf, len);
}

esp_err_t device_config_get_mqtt_client_id(char *buf, size_t len)
{
    return cfg_get(KEY_MQTT_CLIENT_ID, buf, len);
}

esp_err_t device_config_get_mqtt_topic_root(char *buf, size_t len)
{
    return cfg_get(KEY_MQTT_TOPIC_ROOT, buf, len);
}

esp_err_t device_config_set_mqtt_uri(const char *val)
{
    return cfg_set(KEY_MQTT_URI, val);
}

esp_err_t device_config_set_mqtt_client_id(const char *val)
{
    return cfg_set(KEY_MQTT_CLIENT_ID, val);
}

esp_err_t device_config_set_mqtt_topic_root(const char *val)
{
    return cfg_set(KEY_MQTT_TOPIC_ROOT, val);
}

esp_err_t device_config_get_command_topic(char *buf, size_t len)
{
    return cfg_get(KEY_COMMAND_TOPIC, buf, len);
}

esp_err_t device_config_set_command_topic(const char *val)
{
    return cfg_set(KEY_COMMAND_TOPIC, val);
}

esp_err_t device_config_get_status_topic(char *buf, size_t len)
{
    return cfg_get(KEY_STATUS_TOPIC, buf, len);
}

esp_err_t device_config_set_status_topic(const char *val)
{
    return cfg_set(KEY_STATUS_TOPIC, val);
}

esp_err_t device_config_get_timezone(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    char stored[64];
    esp_err_t err = cfg_get(KEY_TIMEZONE, stored, sizeof(stored));
    if (err != ESP_OK) return err;
    return timezone_canonicalize(stored, buf, len);
}

esp_err_t device_config_get_flash_time(uint32_t *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    return nvs_get_u32(s_handle, KEY_FLASH_TIME, out);
}

esp_err_t device_config_get_heartbeat_s(uint32_t *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    return nvs_get_u32(s_handle, KEY_HEARTBEAT_S, out);
}

esp_err_t device_config_set_timezone(const char *val)
{
    char canonical[48];
    esp_err_t err = timezone_canonicalize(val, canonical, sizeof(canonical));
    if (err != ESP_OK) return err;
    return cfg_set(KEY_TIMEZONE, canonical);
}

esp_err_t device_config_get_device_id(char *buf, size_t len)
{
    return cfg_get(KEY_DEVICE_ID, buf, len);
}

esp_err_t device_config_set_device_id(const char *val)
{
    return cfg_set(KEY_DEVICE_ID, val);
}

esp_err_t device_config_get_protocol_id(char *buf, size_t len)
{
    return cfg_get(KEY_PROTOCOL_ID, buf, len);
}

esp_err_t device_config_set_protocol_id(const char *val)
{
    return cfg_set(KEY_PROTOCOL_ID, val);
}

esp_err_t device_config_get_device_name(char *buf, size_t len)
{
    return cfg_get(KEY_DEVICE_NAME, buf, len);
}

esp_err_t device_config_set_device_name(const char *val)
{
    return cfg_set(KEY_DEVICE_NAME, val);
}

esp_err_t device_config_get_device_version(char *buf, size_t len)
{
    return cfg_get(KEY_DEVICE_VERSION, buf, len);
}

esp_err_t device_config_set_device_version(const char *val)
{
    return cfg_set(KEY_DEVICE_VERSION, val);
}

esp_err_t device_config_get_device_firmware(char *buf, size_t len)
{
    return cfg_get(KEY_DEVICE_FIRMWARE, buf, len);
}

esp_err_t device_config_set_device_firmware(const char *val)
{
    return cfg_set(KEY_DEVICE_FIRMWARE, val);
}

esp_err_t device_config_get_firmware_version(char *buf, size_t len)
{
    return cfg_get(KEY_FIRMWARE_VER, buf, len);
}

esp_err_t device_config_set_firmware_version(const char *val)
{
    return cfg_set(KEY_FIRMWARE_VER, val);
}
