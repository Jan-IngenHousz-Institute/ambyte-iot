/* Host stand-in: a mutex that ASSERTS it is never taken twice, so a
 * re-entrant lock in power_protect.c fails the test instead of deadlocking
 * the device. */
#ifndef PP_HOST_SEMPHR_H
#define PP_HOST_SEMPHR_H
#include <assert.h>
#include "FreeRTOS.h"
typedef struct { int taken; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { s->taken = 0; return s; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)t; assert(!s->taken); s->taken = 1; return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { assert(s->taken); s->taken = 0; return pdTRUE; }
#endif
