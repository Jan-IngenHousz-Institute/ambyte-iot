/* Host stand-in (tests/power_protect_host): the esp_err_t subset the
 * power_protect core, glue and command use. Values match ESP-IDF 5.5. */
#ifndef PP_HOST_ESP_ERR_H
#define PP_HOST_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NOT_FINISHED 0x10C
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
const char *esp_err_to_name(esp_err_t err);
#endif
