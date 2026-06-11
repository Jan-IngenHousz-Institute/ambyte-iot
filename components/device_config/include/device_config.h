#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime device configuration stored in NVS namespace "device_cfg".
 * Returns ESP_ERR_NOT_FOUND when a key is absent — caller falls back to
 * compile-time CONFIG_AMBYTE_* defaults.
 * NVS must be initialised (nvs_flash_init) before calling device_config_init.
 */

esp_err_t device_config_init(void);

/* Getters — return ESP_ERR_NOT_FOUND if not set in NVS */
esp_err_t device_config_get_mqtt_uri(char *buf, size_t len);
esp_err_t device_config_get_mqtt_client_id(char *buf, size_t len);
esp_err_t device_config_get_mqtt_topic_root(char *buf, size_t len);
/* Inbound command topic (device subscribes) + reply topic (device publishes).
 * Full, deployment-specific topics independent of mqtt_topic_root. */
esp_err_t device_config_get_command_topic(char *buf, size_t len);
esp_err_t device_config_get_status_topic(char *buf, size_t len);
esp_err_t device_config_get_device_id(char *buf, size_t len);
esp_err_t device_config_get_protocol_id(char *buf, size_t len);
esp_err_t device_config_get_device_name(char *buf, size_t len);
esp_err_t device_config_get_device_version(char *buf, size_t len);
esp_err_t device_config_get_device_firmware(char *buf, size_t len);
esp_err_t device_config_get_firmware_version(char *buf, size_t len);
/* IANA timezone name (e.g. "Europe/Amsterdam") echoed in the MQTT envelope so
 * the cloud derives local-time columns. Optional. */
esp_err_t device_config_get_timezone(char *buf, size_t len);
/* UTC epoch of the provisioning-image build (written by tools/build_nvs_image.py
 * on every build). Boot-time RTC bootstrap: applied only when the RTC is invalid
 * or behind it, so a correct clock is never moved. */
esp_err_t device_config_get_flash_time(uint32_t *out);

/* Setters — persist immediately via nvs_commit */
esp_err_t device_config_set_mqtt_uri(const char *val);
esp_err_t device_config_set_mqtt_client_id(const char *val);
esp_err_t device_config_set_mqtt_topic_root(const char *val);
esp_err_t device_config_set_command_topic(const char *val);
esp_err_t device_config_set_status_topic(const char *val);
esp_err_t device_config_set_device_id(const char *val);
esp_err_t device_config_set_protocol_id(const char *val);
esp_err_t device_config_set_device_name(const char *val);
esp_err_t device_config_set_device_version(const char *val);
esp_err_t device_config_set_device_firmware(const char *val);
esp_err_t device_config_set_firmware_version(const char *val);
esp_err_t device_config_set_timezone(const char *val);

#ifdef __cplusplus
}
#endif
