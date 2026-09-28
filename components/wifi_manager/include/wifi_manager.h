#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(void);
/* Returned by wifi_manager_connect() when the AP rejected the key/identity
 * (auth-class disconnect). Numerically ESP_ERR_WIFI_PASSWORD, so
 * esp_err_to_name() prints that; spelled via ESP_ERR_WIFI_BASE so callers need
 * not depend on esp_wifi.h (wifi_manager.c static-asserts the equality). */
#define WIFI_MANAGER_ERR_AUTH_REJECTED (ESP_ERR_WIFI_BASE + 11)

/* Apply + persist new credentials and wait up to 10 s for an IP. Returns
 * ESP_OK on GOT_IP, WIFI_MANAGER_ERR_AUTH_REJECTED if the AP rejected the
 * key/identity (often a wrong password, sometimes transient), ESP_FAIL on any
 * other reported failure, ESP_ERR_TIMEOUT if nothing was reported in time, or
 * the driver's error if the config/connect call itself failed. An auth
 * rejection or timeout does not stop the manager: it keeps retrying the new
 * credentials on its bounded backoff. */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_connect_configured(void);
esp_err_t wifi_manager_connect_stored(void);
/* Same as wifi_manager_connect_stored() but returns as soon as the connect is
 * initiated (no blocking wait for an IP). The CONNECTED/FAILED result arrives via
 * the Wi-Fi events; the background reconnect logic keeps retrying. Use this on the
 * boot path so a missing AP can't stall the rest of init (sensors, SD, schedule). */
esp_err_t wifi_manager_connect_stored_async(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_is_provisioned(bool *out_provisioned);

/**
 * @brief Clear Wi-Fi credentials and provisioning state, then reboot.
 *        Clears NVS namespace "wifi_prov" and restores Wi-Fi factory config.
 *        Never returns — calls esp_restart() internally.
 */
esp_err_t wifi_manager_clear_provisioning(void);

#ifdef __cplusplus
}
#endif
