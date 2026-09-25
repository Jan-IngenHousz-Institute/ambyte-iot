#ifndef EVQ_HOST_TASK_H
#define EVQ_HOST_TASK_H
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t t);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t t);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
#endif
