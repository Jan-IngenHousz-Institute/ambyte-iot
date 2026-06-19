#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_connect_configured(void);
esp_err_t wifi_manager_connect_stored(void);
/* Same as wifi_manager_connect_stored() but returns as soon as the connect is
 * initiated (no blocking wait for an IP). The CONNECTED/FAILED result arrives via
 * the Wi-Fi events; the background reconnect logic keeps retrying. Use this on the
 * boot path so a missing AP can't stall the rest of init (sensors, SD, Lua). */
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
