#pragma once
/*
 * diag_log_port.h — the four platform facts the capture hook needs, and
 * nothing else: one spinlock, a lock-free monotonic clock, "am I in an ISR",
 * and "which task am I". On the target they are FreeRTOS/esp_timer calls that
 * never block; tests/diag_host supplies pthread versions (DIAG_LOG_HOST) so
 * the production ring, rate limiter and hook run under real concurrency.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef DIAG_LOG_HOST

void    diag_port_lock(void);
void    diag_port_unlock(void);
int64_t diag_port_uptime_us(void);
bool    diag_port_in_isr(void);
void   *diag_port_task(void);

#else

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* _SAFE: correct from task or ISR context (the hook bails out of ISRs
 * before taking it, but the counters are bumped under it in that path). */
extern portMUX_TYPE g_diag_log_mux;
#define diag_port_lock()     portENTER_CRITICAL_SAFE(&g_diag_log_mux)
#define diag_port_unlock()   portEXIT_CRITICAL_SAFE(&g_diag_log_mux)
#define diag_port_uptime_us() ((int64_t)esp_timer_get_time())   /* lock-free */
#define diag_port_in_isr()   (xPortInIsrContext() != 0)
#define diag_port_task()     ((void *)xTaskGetCurrentTaskHandle())

#endif
