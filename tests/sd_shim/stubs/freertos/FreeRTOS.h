#ifndef SD_SHIM_FREERTOS_H
#define SD_SHIM_FREERTOS_H
/* Deterministic lock-step FreeRTOS stand-in (tests/sd_shim/host_rtos.c): the
 * one created task runs only when the driver grants it a step, and every
 * vTaskDelay is a scheduling point that advances a fake 1-ms tick. */
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void host_crit_enter(void);
void host_crit_exit(void);
#define portENTER_CRITICAL_SAFE(m) ((void)(m), host_crit_enter())
#define portEXIT_CRITICAL_SAFE(m)  ((void)(m), host_crit_exit())
#define portENTER_CRITICAL(m)      ((void)(m), host_crit_enter())
#define portEXIT_CRITICAL(m)       ((void)(m), host_crit_exit())
static inline bool xPortInIsrContext(void) { return false; }
#endif
