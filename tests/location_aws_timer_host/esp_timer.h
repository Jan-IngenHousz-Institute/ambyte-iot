#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_stop(esp_timer_handle_t);
esp_err_t esp_timer_start_once(esp_timer_handle_t, uint64_t);
bool esp_timer_is_active(esp_timer_handle_t);
