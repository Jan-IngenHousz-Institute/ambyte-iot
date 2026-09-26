#ifndef EVQ_HOST_SEMPHR_H
#define EVQ_HOST_SEMPHR_H
#include "freertos/FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
#endif
