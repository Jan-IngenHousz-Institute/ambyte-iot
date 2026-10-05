#pragma once
#include "freertos/FreeRTOS.h"
typedef struct hqueue *QueueHandle_t;
typedef struct { void *opaque[8]; } StaticQueue_t;
QueueHandle_t xQueueCreateStatic(UBaseType_t len, UBaseType_t item_size, uint8_t *storage, StaticQueue_t *q);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t xQueueReset(QueueHandle_t q);
