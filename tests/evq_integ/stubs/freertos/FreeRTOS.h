/* Host FreeRTOS shim (tests/evq_integ/rtos_shim.c): pthread-backed tasks on a
 * cooperative single-core scheduler over a VIRTUAL tick clock. A task runs
 * until it blocks (delay / notify-take / contended semaphore / yield) or wakes
 * a higher-priority task; the clock advances only when no task is ready. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define configTICK_RATE_HZ 100   /* CONFIG_FREERTOS_HZ=100 (sdkconfig.esp32-s3-devkitm-1) */
#define portTICK_PERIOD_MS (1000 / configTICK_RATE_HZ)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * (uint64_t)configTICK_RATE_HZ) / 1000U))
typedef struct { int depth; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
void h_enter_critical(portMUX_TYPE *m);
void h_exit_critical(portMUX_TYPE *m);
#define portENTER_CRITICAL(m) h_enter_critical(m)
#define portEXIT_CRITICAL(m)  h_exit_critical(m)
