/* Host stand-ins for the ESP-IDF declarations components/diag/diag_boot.c uses
 * (values as in the pinned framework-espidf@3.50500.0). */
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_NVS_NOT_FOUND 0x1102
const char *esp_err_to_name(esp_err_t err);
