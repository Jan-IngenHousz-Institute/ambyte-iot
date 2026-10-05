#pragma once
#include "freertos/FreeRTOS.h"
typedef struct hsem *SemaphoreHandle_t;
typedef struct { void *opaque[8]; } StaticSemaphore_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
