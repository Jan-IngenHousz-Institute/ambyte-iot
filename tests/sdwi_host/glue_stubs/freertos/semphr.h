#pragma once
/* pthread-backed FreeRTOS mutex stand-in for the sd_diag production-glue
 * concurrency test. `sdwi_mtx_contended` counts takes that found the mutex
 * held, so the test can prove a second caller is really WAITING (not just
 * scheduled late) before it releases the first one. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define portMAX_DELAY ((TickType_t)0xffffffffu)
#define pdTRUE 1
typedef struct { pthread_mutex_t m; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
extern atomic_int sdwi_mtx_contended;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b)
{
    pthread_mutex_init(&b->m, NULL);
    return b;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t)
{
    (void)t;
    if (pthread_mutex_trylock(&h->m) != 0) {
        atomic_fetch_add(&sdwi_mtx_contended, 1);
        pthread_mutex_lock(&h->m);
    }
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h)
{
    pthread_mutex_unlock(&h->m);
    return pdTRUE;
}
