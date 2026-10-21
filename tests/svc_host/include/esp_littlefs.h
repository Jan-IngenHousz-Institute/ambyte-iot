#pragma once
#include <stddef.h>
#include "esp_err.h"
esp_err_t esp_littlefs_info(const char *partition, size_t *total, size_t *used);
