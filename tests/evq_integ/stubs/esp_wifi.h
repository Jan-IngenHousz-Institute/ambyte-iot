#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP = 1 } wifi_interface_t;
esp_err_t esp_wifi_get_mac(wifi_interface_t ifx, uint8_t mac[6]);
