/* Host stub: the esp_wifi surface wifi_manager.c uses. Reason codes and error
 * codes are copied from ESP-IDF 5.5 (esp_wifi_types_generic.h / esp_wifi.h). */
#ifndef WM_STUB_ESP_WIFI_H
#define WM_STUB_ESP_WIFI_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_ERR_WIFI_STATE       (ESP_ERR_WIFI_BASE + 6)
#define ESP_ERR_WIFI_CONN        (ESP_ERR_WIFI_BASE + 7)
#define ESP_ERR_WIFI_PASSWORD    (ESP_ERR_WIFI_BASE + 11)
#define ESP_ERR_WIFI_NOT_CONNECT (ESP_ERR_WIFI_BASE + 15)
typedef enum {
    WIFI_REASON_UNSPECIFIED            = 1,
    WIFI_REASON_AUTH_EXPIRE            = 2,
    WIFI_REASON_AUTH_LEAVE             = 3,
    WIFI_REASON_ASSOC_LEAVE            = 8,
    WIFI_REASON_ASSOC_NOT_AUTHED       = 9,
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
    WIFI_REASON_802_1X_AUTH_FAILED     = 23,
    WIFI_REASON_BEACON_TIMEOUT         = 200,
    WIFI_REASON_NO_AP_FOUND            = 201,
    WIFI_REASON_AUTH_FAIL              = 202,
    WIFI_REASON_ASSOC_FAIL             = 203,
    WIFI_REASON_HANDSHAKE_TIMEOUT      = 204,
    WIFI_REASON_CONNECTION_FAIL        = 205,
} wifi_err_reason_t;
typedef enum { WIFI_IF_STA = 0 } wifi_interface_t;
typedef enum { WIFI_MODE_STA = 1 } wifi_mode_t;
typedef enum { WIFI_PS_MIN_MODEM = 1 } wifi_ps_type_t;
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3 } wifi_auth_mode_t;
typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    struct { wifi_auth_mode_t authmode; } threshold;
    struct { bool capable; bool required; } pmf_cfg;
} wifi_sta_config_t;
typedef union { wifi_sta_config_t sta; } wifi_config_t;
typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() { 0 }
/* Field order/widths as in ESP-IDF 5.5 (reason is a uint8_t there). */
typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint8_t reason;
    int8_t rssi;
} wifi_event_sta_disconnected_t;
enum {
    WIFI_EVENT_STA_START = 2,
    WIFI_EVENT_STA_CONNECTED = 4,
    WIFI_EVENT_STA_DISCONNECTED = 5,
};
esp_err_t esp_wifi_init(const wifi_init_config_t *cfg);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);
esp_err_t esp_wifi_set_config(wifi_interface_t iface, wifi_config_t *conf);
esp_err_t esp_wifi_get_config(wifi_interface_t iface, wifi_config_t *conf);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_restore(void);
#endif
