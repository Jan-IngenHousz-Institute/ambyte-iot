#pragma once
#include "../../include/freertos/ringbuf.h"
RingbufHandle_t xRingbufferCreate(size_t, RingbufferType_t);
BaseType_t xRingbufferSendFromISR(RingbufHandle_t, const void *, size_t, BaseType_t *);
BaseType_t xRingbufferSend(RingbufHandle_t, const void *, size_t, TickType_t);
void *xRingbufferReceiveUpTo(RingbufHandle_t, size_t *, TickType_t, size_t);
void *xRingbufferReceiveUpToFromISR(RingbufHandle_t, size_t *, size_t);
void vRingbufferReturnItem(RingbufHandle_t, void *);
void vRingbufferReturnItemFromISR(RingbufHandle_t, void *, BaseType_t *);
void vRingbufferGetInfo(RingbufHandle_t, UBaseType_t *, UBaseType_t *, UBaseType_t *, UBaseType_t *, UBaseType_t *);
size_t xRingbufferGetCurFreeSize(RingbufHandle_t);
void vRingbufferDelete(RingbufHandle_t);
