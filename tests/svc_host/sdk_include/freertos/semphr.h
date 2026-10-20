#pragma once
#include "../../include/freertos/semphr.h"
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t, BaseType_t *);
void vSemaphoreDelete(SemaphoreHandle_t);

#include "freertos/task.h"
#include "esp_heap_caps.h"
