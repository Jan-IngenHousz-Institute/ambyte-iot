#pragma once
#include "../../host_stubs/esp_err.h"
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(esp_err_t code);
