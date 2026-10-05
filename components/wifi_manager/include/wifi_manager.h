#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(void);
/* Manager-owned result codes, in a range no ESP-IDF component uses (0x8A00),
 * so they can never be confused with a driver error. In particular this is NOT
 * ESP_ERR_WIFI_PASSWORD: esp_wifi_set_config() returns that for a malformed
 * password before anything is saved or attempted. */
#define WIFI_MANAGER_ERR_BASE 0x8A00
/* The credentials were saved and a connect attempted, and the AP rejected the
 * key/identity (auth-class disconnect: often a wrong password, sometimes
 * transient). The manager keeps retrying in the background. */
#define WIFI_MANAGER_ERR_AUTH_REJECTED (WIFI_MANAGER_ERR_BASE + 1)
/* The attempt failed and the manager has ENDED the request: no retry is
 * pending (e.g. the retry timer could not be armed). Re-run the join, or
 * reboot. */
#define WIFI_MANAGER_ERR_NOT_RETRYING (WIFI_MANAGER_ERR_BASE + 2)
/* The request was ended because an earlier connect attempt (or a superseded
 * link) is still unresolved in the Wi-Fi driver after being kicked. ESP-IDF
 * 5.5 provides no event barrier to hand the attempt slot over safely, so the
 * manager does not guess. Nothing is retrying; if the earlier outcome never
 * arrives, reboot to recover. */
#define WIFI_MANAGER_ERR_DRIVER_UNRESOLVED (WIFI_MANAGER_ERR_BASE + 3)

/* esp_err_to_name() that also knows WIFI_MANAGER_ERR_* codes. */
const char *wifi_manager_err_to_name(esp_err_t err);

/* Apply + persist new credentials and wait up to 10 s for an IP. Returns:
 *  - ESP_OK on GOT_IP;
 *  - WIFI_MANAGER_ERR_AUTH_REJECTED if the AP rejected the key/identity;
 *  - ESP_FAIL on any other reported failure;
 *  - ESP_ERR_TIMEOUT if no result arrived in time (possibly associated with
 *    DHCP still pending);
 *  - WIFI_MANAGER_ERR_NOT_RETRYING if the attempt failed and the request was
 *    ended (nothing pending);
 *  - the driver's own error if a config/disconnect/connect CALL failed.
 * After AUTH_REJECTED, ESP_FAIL or ESP_ERR_TIMEOUT the manager still owns the
 * request and keeps retrying the new credentials on its bounded backoff. After
 * NOT_RETRYING or a driver call error nothing retries (a config error saved
 * nothing) and the caller should re-run the join. */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_connect_configured(void);
esp_err_t wifi_manager_connect_stored(void);
/* Same as wifi_manager_connect_stored() but returns as soon as the connect is
 * initiated (no blocking wait for an IP). The CONNECTED/FAILED result arrives via
 * the Wi-Fi events; the background reconnect logic keeps retrying. Use this on the
 * boot path so a missing AP can't stall the rest of init (sensors, SD, schedule).
 * A transient driver error on the first connect call is handed to the same
 * bounded retry and still returns ESP_OK; an error return means nothing will
 * retry (not started, or a configuration error such as no stored SSID).
 * Re-entry is safe: it supersedes the previous request, and an attempt still
 * unresolved is never overwritten (the new one is deferred to the retry
 * timer until it resolves); if that deferral cannot arm a retry the request
 * ends and WIFI_MANAGER_ERR_NOT_RETRYING is returned. */
esp_err_t wifi_manager_connect_stored_async(void);
bool wifi_manager_is_connected(void);
/* True while the station's association belongs to the CURRENT connect request.
 * ESP events reach every registered handler, so an application GOT_IP handler
 * must check this before starting link services (SNTP, MQTT): a superseded
 * request's late association is not the current link and is about to be torn
 * down. Ownership is fixed at STA_CONNECTED, which always precedes GOT_IP, so
 * the answer does not depend on handler order. */
bool wifi_manager_link_is_current(void);
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
