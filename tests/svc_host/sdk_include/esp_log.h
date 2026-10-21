#pragma once
#include <stdarg.h>
#include "esp_err.h"
typedef int (*vprintf_like_t)(const char *,va_list);
#define ESP_LOGE(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGI(tag,...) ((void)(tag))
