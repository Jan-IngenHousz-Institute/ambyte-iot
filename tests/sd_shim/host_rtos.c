/* Lock-step FreeRTOS stand-in (see stubs/freertos/FreeRTOS.h). The writer
 * thread blocks in every vTaskDelay until the driver grants a step; the driver
 * waits until the writer is blocked again, so the two never run concurrently
 * and every run is deterministic. The critical section is a real mutex anyway. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static pthread_mutex_t s_crit = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_c = PTHREAD_COND_INITIALIZER;
static pthread_t s_task;
static int  s_have_task;
static int  s_grant;
static int  s_waiting;
static int  s_forever;
static volatile TickType_t s_tick;
int  g_refs_at_delay_violations;      /* writer delayed while holding an SD io ref */
extern int g_sd_refs;

void host_crit_enter(void) { pthread_mutex_lock(&s_crit); }
void host_crit_exit(void) { pthread_mutex_unlock(&s_crit); }

typedef struct { TaskFunction_t fn; void *arg; } tramp_t;
static tramp_t s_tr;
static void *tramp(void *a) { (void)a; s_tr.fn(s_tr.arg); return NULL; }

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, unsigned prio, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)prio;
    s_tr.fn = fn; s_tr.arg = arg;
    s_have_task = 1;
    pthread_create(&s_task, NULL, tramp, NULL);
    if (out) *out = (TaskHandle_t)(uintptr_t)1;
    /* run the task until its first scheduling point */
    pthread_mutex_lock(&s_m);
    while (!s_waiting) pthread_cond_wait(&s_c, &s_m);
    pthread_mutex_unlock(&s_m);
    return pdPASS;
}

static int is_task(void) { return s_have_task && pthread_equal(pthread_self(), s_task); }

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return is_task() ? (TaskHandle_t)(uintptr_t)1 : (TaskHandle_t)(uintptr_t)2; }
TickType_t xTaskGetTickCount(void) { return s_tick; }

void host_step(int n);

void vTaskDelay(TickType_t ticks)
{
    if (is_task()) {
        if (g_sd_refs != 0) g_refs_at_delay_violations++;
        pthread_mutex_lock(&s_m);
        if (ticks == portMAX_DELAY) s_forever = 1; else s_tick += ticks;
        s_waiting = 1;
        pthread_cond_broadcast(&s_c);
        for (;;) {
            if (!s_forever && s_grant > 0) { s_grant--; break; }
            pthread_cond_wait(&s_c, &s_m);
        }
        s_waiting = 0;
        pthread_mutex_unlock(&s_m);
        return;
    }
    /* driver-side wait loop (pause / prepare_shutdown): time passes, and the
     * writer gets a turn */
    s_tick += ticks;
    host_step(1);
}

/* Grant the writer n scheduling steps; returns once it is blocked again. */
void host_step(int n)
{
    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&s_m);
        if (s_forever || !s_have_task) { pthread_mutex_unlock(&s_m); return; }
        s_grant++;
        pthread_cond_broadcast(&s_c);
        while (!(s_waiting && s_grant == 0) && !s_forever) pthread_cond_wait(&s_c, &s_m);
        /* s_waiting may still be set from before the grant was consumed: wait for a fresh block */
        pthread_mutex_unlock(&s_m);
    }
}

TickType_t host_tick(void) { return s_tick; }
void host_advance(TickType_t t) { s_tick += t; }

/* ── esp_log ── */
static int console_vprintf(const char *fmt, va_list ap);
static vprintf_like_t s_vprintf = console_vprintf;
FILE *g_console;
static int console_vprintf(const char *fmt, va_list ap)
{
    return g_console ? vfprintf(g_console, fmt, ap) : 0;
}
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    vprintf_like_t old = s_vprintf;
    s_vprintf = func;
    return old;
}
void host_log_write(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    s_vprintf(fmt, ap);
    va_end(ap);
}

void host_log_vwrite(const char *fmt, va_list ap)
{
    s_vprintf(fmt, ap);
}
