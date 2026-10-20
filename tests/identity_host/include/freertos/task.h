#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
    unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle, int core);
TickType_t xTaskGetTickCount(void);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
