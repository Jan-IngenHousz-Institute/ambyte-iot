/* Host stub (wifi_manager harness): values match ESP-IDF 5.5 esp_err.h. */
#ifndef WM_STUB_ESP_ERR_H
#define WM_STUB_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_WIFI_BASE 0x3000
#define ESP_ERR_MESH_BASE 0x4000
const char *esp_err_to_name(esp_err_t code);
#endif
