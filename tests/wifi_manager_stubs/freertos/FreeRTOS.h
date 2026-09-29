#ifndef WM_STUB_FREERTOS_H
#define WM_STUB_FREERTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef long BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define BIT0 0x00000001U
#define BIT1 0x00000002U
#define BIT2 0x00000004U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
void vTaskDelay(TickType_t ticks);
#endif
