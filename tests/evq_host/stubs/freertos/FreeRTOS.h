#ifndef EVQ_HOST_FREERTOS_H
#define EVQ_HOST_FREERTOS_H
/* Single-threaded host FreeRTOS surface for the storage harness. Ticks are a
 * test clock (1 tick = 1 ms) advanced by the driver; the mutex asserts it is
 * never re-taken by its holder (a self-deadlock on target). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef struct { int held; int depth_guard; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif
