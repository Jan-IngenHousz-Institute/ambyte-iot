#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint8_t StackType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#ifndef ESP_ERR_TIMEOUT
#define ESP_ERR_TIMEOUT 0x107
#endif
void identity_host_enter(portMUX_TYPE *mux);
void identity_host_exit(portMUX_TYPE *mux);
#define taskENTER_CRITICAL(mux) identity_host_enter(mux)
#define taskEXIT_CRITICAL(mux) identity_host_exit(mux)
