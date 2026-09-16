#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
