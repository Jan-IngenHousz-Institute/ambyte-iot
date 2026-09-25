#pragma once
#include "freertos/FreeRTOS.h"
typedef struct htask *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t  xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth, void *arg,
                        UBaseType_t prio, TaskHandle_t *out);
BaseType_t  xTaskNotifyGive(TaskHandle_t t);
uint32_t    ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks);
void        vTaskDelay(TickType_t ticks);
TickType_t  xTaskGetTickCount(void);
void        h_task_yield(void);
#define taskYIELD() h_task_yield()
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
