#ifndef WM_STUB_SEMPHR_H
#define WM_STUB_SEMPHR_H
#include "freertos/FreeRTOS.h"
typedef struct wm_stub_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t m);
#endif
