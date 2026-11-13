#pragma once
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, int ticks) { (void)s; (void)ticks; return 0; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
