#pragma once

#include <stdbool.h>
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
/* Canonical, firmware-supported IANA timezone name (e.g. "Europe/Amsterdam")
 * echoed in the MQTT envelope so the cloud derives local-time columns.
 * Optional. Invalid stored values fail closed instead of being returned. */
esp_err_t device_config_get_timezone(char *buf, size_t len);
/* UTC epoch of the provisioning-image build (written by tools/build_nvs_image.py
 * on every build). Boot-time RTC bootstrap: applied only when the RTC is invalid
 * or behind it, so a correct clock is never moved. */
esp_err_t device_config_get_flash_time(uint32_t *out);
/* STATUS heartbeat period in seconds (sync_runner). Optional; caller defaults
 * (300 s) when unset. 0 disables the heartbeat. */
esp_err_t device_config_get_heartbeat_s(uint32_t *out);

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
/* Trims/canonicalizes supported values (including legacy AMT ->
 * Europe/Amsterdam) and rejects unknown zones with ESP_ERR_INVALID_ARG. */
esp_err_t device_config_set_timezone(const char *val);

/* Transport gzip of the published v3 measurement `sample`
 * (`_sample_encoding: "gzip+base64"`). Absent defaults to DISABLED so a fleet
 * keeps publishing plain JSON until the OpenJII ingest confirms gzip support;
 * flip per device with `cfg set publish_gzip 1` (no reboot needed). The bool
 * reader is RAM-cached for the per-publish check in the MQTT drain path; the
 * string get/set pair serves the CLI cfg table ("0"/"1", set also accepts
 * on/off/true/false). */
bool device_config_publish_gzip_enabled(void);
esp_err_t device_config_get_publish_gzip(char *buf, size_t len);
esp_err_t device_config_set_publish_gzip(const char *val);

#ifdef __cplusplus
}
#endif
