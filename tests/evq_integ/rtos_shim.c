/*
 * rtos_shim.c — pthread-backed FreeRTOS shim with a VIRTUAL clock.
 *
 * Model: every xTaskCreate()d task (and the test driver, registered with
 * h_shim_start) is a real pthread, but exactly one of them runs at a time — a
 * single-core FreeRTOS with the kernel's own rule for who runs: the highest-
 * priority READY task, FIFO among equals. A running task keeps the CPU until it
 * blocks (vTaskDelay, ulTaskNotifyTake, a contended xSemaphoreTake), yields
 * (taskYIELD), or wakes a strictly higher-priority task (notify/give/create),
 * exactly the points where FreeRTOS would switch. Virtual time advances ONLY
 * when no task is ready: it jumps to the earliest pending timeout. Production
 * task bodies therefore run unmodified, and every run is deterministic.
 *
 * Limits (stated in the evidence): no preemption between shim calls and no
 * second core, so interleavings inside a critical region that never calls the
 * kernel are not explored; I/O takes zero virtual time.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "shim_api.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define TICK_US (1000000ULL / configTICK_RATE_HZ)
#define MAX_TASKS 24
#define MAX_WAITERS 16

typedef enum { T_READY = 0, T_BLOCKED, T_DEAD } tstate_t;
typedef enum { W_NONE = 0, W_DELAY, W_NOTIFY, W_SEM } wkind_t;

struct hsem;

struct htask {
    int         id;
    char        name[24];
    int         prio;
    pthread_t   th;
    pthread_cond_t cv;
    tstate_t    state;
    wkind_t     wait;
    bool        timed;
    uint64_t    wake_us;
    bool        timed_out;
    uint64_t    ready_seq;
    uint32_t    notify;
    char        last_notifier[24];
    struct hsem *sem_wait;
    TaskFunction_t fn;
    void       *arg;
    int         crit_depth;
    bool        traced;
};

struct hsem {
    struct htask *owner;
    struct htask *waiters[MAX_WAITERS];
    int           nwait;
    char          name[24];
};

struct hqueue {
    uint8_t *storage;
    size_t   len, isz, head, count;
};

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static struct htask *g_tasks[MAX_TASKS];
static int           g_ntasks;
static struct htask *g_cur;
static uint64_t      g_vt_us;
static uint64_t      g_seq;
static __thread struct htask *t_self;

static struct hsem   g_sems[32];
static int           g_nsems;
static struct hqueue g_queues[8];
static int           g_nqueues;

/* ── counters exported to the harness ── */
h_shim_stats_t h_shim_stats;

/* ── virtual wall clock ── */
static int64_t g_epoch_base_s = 1790380800LL;   /* 2026-09-26T00:00:00Z */

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void dump_tasks_locked(void)
{
    fprintf(stderr, "[shim] vt=%llu us tasks:\n", (unsigned long long)g_vt_us);
    for (int i = 0; i < g_ntasks; i++) {
        struct htask *t = g_tasks[i];
        fprintf(stderr, "  %-14s prio=%d state=%d wait=%d timed=%d wake=%llu notify=%u%s\n",
                t->name, t->prio, (int)t->state, (int)t->wait, (int)t->timed,
                (unsigned long long)t->wake_us, t->notify, t == g_cur ? " <cur" : "");
    }
}
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[shim] FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    dump_tasks_locked();
    abort();
}

static uint64_t tick_now_locked(void) { return g_vt_us / TICK_US; }

static void make_ready_locked(struct htask *t)
{
    t->state = T_READY;
    t->wait = W_NONE;
    t->ready_seq = ++g_seq;
}

static void sem_remove_waiter_locked(struct hsem *s, struct htask *t)
{
    for (int i = 0; i < s->nwait; i++) {
        if (s->waiters[i] == t) {
            memmove(&s->waiters[i], &s->waiters[i + 1], (size_t)(s->nwait - i - 1) * sizeof s->waiters[0]);
            s->nwait--;
            return;
        }
    }
}

static struct htask *pick_next_locked(void)
{
    for (;;) {
        struct htask *best = NULL;
        for (int i = 0; i < g_ntasks; i++) {
            struct htask *t = g_tasks[i];
            if (t->state != T_READY) continue;
            if (best == NULL || t->prio > best->prio ||
                (t->prio == best->prio && t->ready_seq < best->ready_seq)) best = t;
        }
        if (best != NULL) return best;
        uint64_t min = UINT64_MAX;
        for (int i = 0; i < g_ntasks; i++) {
            struct htask *t = g_tasks[i];
            if (t->state == T_BLOCKED && t->timed && t->wake_us < min) min = t->wake_us;
        }
        if (min == UINT64_MAX) die("deadlock: no ready task and no pending timeout");
        if (min > g_vt_us) {
            g_vt_us = min;
            h_shim_stats.time_advances++;
        }
        for (int i = 0; i < g_ntasks; i++) {
            struct htask *t = g_tasks[i];
            if (t->state == T_BLOCKED && t->timed && t->wake_us <= g_vt_us) {
                if (t->wait == W_SEM && t->sem_wait != NULL) {
                    sem_remove_waiter_locked(t->sem_wait, t);
                    t->sem_wait = NULL;
                }
                t->timed_out = true;
                make_ready_locked(t);
            }
        }
    }
}

/* Caller holds G; `me` has already been put in READY (yield) or BLOCKED. */
static void switch_away_locked(struct htask *me)
{
    struct htask *next = pick_next_locked();
    if (next != me) {
        h_shim_stats.switches++;
        g_cur = next;
        pthread_cond_signal(&next->cv);
        while (g_cur != me) pthread_cond_wait(&me->cv, &G);
    }
}

static struct htask *self_or_die(void)
{
    if (t_self == NULL) die("kernel call from a thread that is not a shim task");
    if (g_cur != t_self) die("task %s runs while %s holds the CPU", t_self->name, g_cur ? g_cur->name : "?");
    return t_self;
}

/* Block the calling task for up to `ticks`; returns true when it timed out. */
static bool block_locked(struct htask *me, wkind_t w, TickType_t ticks)
{
    if (me->crit_depth > 0) die("task %s blocks inside a portMUX critical section", me->name);
    me->wait = w;
    me->timed = ticks != portMAX_DELAY;
    me->wake_us = me->timed ? (tick_now_locked() + (uint64_t)ticks) * TICK_US : 0;
    me->timed_out = false;
    me->state = T_BLOCKED;
    switch_away_locked(me);
    return me->timed_out;
}

static void maybe_preempt_locked(struct htask *me, struct htask *woken)
{
    if (woken->prio > me->prio) {
        me->state = T_READY;
        me->ready_seq = ++g_seq;
        switch_away_locked(me);
    }
}

/* ── task lifecycle ── */

static struct htask *new_task_locked(const char *name, int prio)
{
    if (g_ntasks >= MAX_TASKS) die("too many tasks");
    struct htask *t = calloc(1, sizeof *t);
    if (t == NULL) die("oom");
    t->id = g_ntasks;
    snprintf(t->name, sizeof t->name, "%s", name ? name : "task");
    t->prio = prio;
    pthread_cond_init(&t->cv, NULL);
    for (int i = 0; i < h_shim_stats.ntraced; i++) {
        if (strcmp(h_shim_stats.traced[i], t->name) == 0) t->traced = true;
    }
    g_tasks[g_ntasks++] = t;
    return t;
}

void h_shim_trace_task(const char *name)
{
    if (h_shim_stats.ntraced < (int)(sizeof h_shim_stats.traced / sizeof h_shim_stats.traced[0])) {
        snprintf(h_shim_stats.traced[h_shim_stats.ntraced++], 24, "%s", name);
    }
}

void h_shim_start(const char *name, int prio)
{
    pthread_mutex_lock(&G);
    struct htask *t = new_task_locked(name, prio);
    t->th = pthread_self();
    t->state = T_READY;
    t_self = t;
    g_cur = t;
    pthread_mutex_unlock(&G);
}

static void *thread_entry(void *arg)
{
    struct htask *t = arg;
    pthread_mutex_lock(&G);
    t_self = t;
    while (g_cur != t) pthread_cond_wait(&t->cv, &G);
    pthread_mutex_unlock(&G);
    t->fn(t->arg);
    pthread_mutex_lock(&G);
    t->state = T_DEAD;
    struct htask *next = pick_next_locked();
    g_cur = next;
    pthread_cond_signal(&next->cv);
    pthread_mutex_unlock(&G);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth, void *arg,
                       UBaseType_t prio, TaskHandle_t *out)
{
    (void)stack_depth;
    pthread_mutex_lock(&G);
    struct htask *me = self_or_die();
    struct htask *t = new_task_locked(name, (int)prio);
    t->fn = fn;
    t->arg = arg;
    make_ready_locked(t);
    if (out != NULL) *out = t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);   /* host frames + sanitizer redzones */
    if (pthread_create(&t->th, &attr, thread_entry, t) != 0) die("pthread_create failed");
    pthread_attr_destroy(&attr);
    pthread_detach(t->th);
    maybe_preempt_locked(me, t);
    pthread_mutex_unlock(&G);
    return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return t_self; }

const char *h_task_name(void) { return t_self ? t_self->name : "?"; }

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 4096; }

/* ── notifications ── */

BaseType_t xTaskNotifyGive(TaskHandle_t t)
{
    pthread_mutex_lock(&G);
    struct htask *me = self_or_die();
    t->notify++;
    snprintf(t->last_notifier, sizeof t->last_notifier, "%s", me->name);
    if (t->traced) h_trace_locked_ext(g_vt_us, me->name, "notify", t->name, (int64_t)t->notify);
    if (t->state == T_BLOCKED && t->wait == W_NOTIFY) {
        make_ready_locked(t);
        maybe_preempt_locked(me, t);
    }
    pthread_mutex_unlock(&G);
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks)
{
    pthread_mutex_lock(&G);
    struct htask *me = self_or_die();
    bool timed_out = false;
    if (me->notify == 0 && ticks > 0) timed_out = block_locked(me, W_NOTIFY, ticks);
    uint32_t v = me->notify;
    if (v > 0) {
        if (clear_on_exit) me->notify = 0; else me->notify--;
    }
    if (me->traced) {
        h_trace_locked_ext(g_vt_us, me->name, "wake",
                           v > 0 ? "notify" : (timed_out ? "timeout" : "poll"), (int64_t)v);
    }
    pthread_mutex_unlock(&G);
    return v;
}

void vTaskDelay(TickType_t ticks)
{
    pthread_mutex_lock(&G);
    struct htask *me = self_or_die();
    if (ticks == 0) {
        me->state = T_READY;
        me->ready_seq = ++g_seq;
        switch_away_locked(me);
    } else {
        (void)block_locked(me, W_DELAY, ticks);
    }
    pthread_mutex_unlock(&G);
}

void h_task_yield(void)
{
    pthread_mutex_lock(&G);
    struct htask *me = self_or_die();
    me->state = T_READY;
    me->ready_seq = ++g_seq;
    switch_away_locked(me);
    pthread_mutex_unlock(&G);
}

TickType_t xTaskGetTickCount(void)
{
    pthread_mutex_lock(&G);
    TickType_t t = (TickType_t)tick_now_locked();
    pthread_mutex_unlock(&G);
    return t;
}

int64_t esp_timer_get_time(void)
{
    pthread_mutex_lock(&G);
    int64_t v = (int64_t)g_vt_us;
    pthread_mutex_unlock(&G);
    return v;
}

uint64_t h_vt_us(void) { return (uint64_t)esp_timer_get_time(); }

void h_set_epoch_base(int64_t epoch_s) { g_epoch_base_s = epoch_s; }

time_t h_time(time_t *out)
{
    time_t v = (time_t)(g_epoch_base_s + (int64_t)(h_vt_us() / 1000000ULL));
    if (out != NULL) *out = v;
    return v;
}

int h_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    uint64_t us = h_vt_us();
    tv->tv_sec = (time_t)(g_epoch_base_s + (int64_t)(us / 1000000ULL));
    tv->tv_usec = (suseconds_t)(us % 1000000ULL);
    return 0;
}

/* ── critical sections (single core: only a nesting check) ── */

void h_enter_critical(portMUX_TYPE *m)
{
    (void)m;
    if (t_self != NULL) t_self->crit_depth++;
}
void h_exit_critical(portMUX_TYPE *m)
{
    (void)m;
    if (t_self != NULL) {
        if (t_self->crit_depth <= 0) die("portEXIT_CRITICAL without ENTER in %s", t_self->name);
        t_self->crit_depth--;
    }
}

/* ── mutexes ── */

static struct hsem *new_sem(void)
{
    pthread_mutex_lock(&G);
    if (g_nsems >= (int)(sizeof g_sems / sizeof g_sems[0])) die("too many semaphores");
    struct hsem *s = &g_sems[g_nsems++];
    memset(s, 0, sizeof *s);
    pthread_mutex_unlock(&G);
    return s;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { (void)storage; return new_sem(); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return new_sem(); }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    pthread_mutex_lock(&G);
    struct htask *me = self_or_die();
    h_shim_stats.sem_takes++;
    if (h_sd_refs_held() > 0) h_shim_stats.take_while_sd_ref++;
    if (s->owner == NULL) {
        s->owner = me;
        pthread_mutex_unlock(&G);
        return pdTRUE;
    }
    if (s->owner == me) die("task %s re-takes a non-recursive mutex it owns", me->name);
    h_shim_stats.sem_contended++;
    if (ticks == 0) { pthread_mutex_unlock(&G); return pdFALSE; }
    if (s->nwait >= MAX_WAITERS) die("too many semaphore waiters");
    s->waiters[s->nwait++] = me;
    me->sem_wait = s;
    bool timed_out = block_locked(me, W_SEM, ticks);
    me->sem_wait = NULL;
    if (timed_out) h_shim_stats.sem_timeouts++;
    pthread_mutex_unlock(&G);
    return timed_out ? pdFALSE : pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&G);
    struct htask *me = self_or_die();
    if (s->owner != me) die("task %s gives a mutex it does not own", me->name);
    if (s->nwait > 0) {
        struct htask *w = s->waiters[0];
        int wi = 0;
        for (int i = 1; i < s->nwait; i++) if (s->waiters[i]->prio > w->prio) { w = s->waiters[i]; wi = i; }
        memmove(&s->waiters[wi], &s->waiters[wi + 1], (size_t)(s->nwait - wi - 1) * sizeof s->waiters[0]);
        s->nwait--;
        s->owner = w;
        w->sem_wait = NULL;
        make_ready_locked(w);
        maybe_preempt_locked(me, w);
    } else {
        s->owner = NULL;
    }
    pthread_mutex_unlock(&G);
    return pdTRUE;
}

/* ── queues (device_commands uses zero-wait send/peek/receive only) ── */

QueueHandle_t xQueueCreateStatic(UBaseType_t len, UBaseType_t item_size, uint8_t *storage, StaticQueue_t *q)
{
    (void)q;
    pthread_mutex_lock(&G);
    if (g_nqueues >= (int)(sizeof g_queues / sizeof g_queues[0])) die("too many queues");
    struct hqueue *h = &g_queues[g_nqueues++];
    h->storage = storage; h->len = len; h->isz = item_size; h->head = 0; h->count = 0;
    pthread_mutex_unlock(&G);
    return h;
}

static void no_wait_or_die(TickType_t ticks, const char *what)
{
    if (ticks != 0) die("%s with a non-zero wait is outside this shim's model", what);
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    no_wait_or_die(ticks, "xQueueSend");
    pthread_mutex_lock(&G);
    BaseType_t ok = pdFALSE;
    if (q->count < q->len) {
        memcpy(q->storage + ((q->head + q->count) % q->len) * q->isz, item, q->isz);
        q->count++;
        ok = pdTRUE;
    } else {
        h_shim_stats.queue_full++;
    }
    pthread_mutex_unlock(&G);
    return ok;
}

BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t ticks)
{
    no_wait_or_die(ticks, "xQueuePeek");
    pthread_mutex_lock(&G);
    BaseType_t ok = pdFALSE;
    if (q->count > 0) { memcpy(item, q->storage + q->head * q->isz, q->isz); ok = pdTRUE; }
    pthread_mutex_unlock(&G);
    return ok;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    no_wait_or_die(ticks, "xQueueReceive");
    pthread_mutex_lock(&G);
    BaseType_t ok = pdFALSE;
    if (q->count > 0) {
        memcpy(item, q->storage + q->head * q->isz, q->isz);
        q->head = (q->head + 1) % q->len;
        q->count--;
        ok = pdTRUE;
    }
    pthread_mutex_unlock(&G);
    return ok;
}

BaseType_t xQueueReset(QueueHandle_t q)
{
    pthread_mutex_lock(&G);
    q->head = 0; q->count = 0;
    pthread_mutex_unlock(&G);
    return pdPASS;
}

/* ── trace ── */

static h_trace_t *g_trace;
static size_t     g_ntrace, g_captrace;

void h_trace_locked_ext(uint64_t vt, const char *task, const char *kind, const char *a, int64_t n)
{
    if (g_ntrace == g_captrace) {
        g_captrace = g_captrace ? g_captrace * 2 : 4096;
        g_trace = realloc(g_trace, g_captrace * sizeof *g_trace);
        if (g_trace == NULL) abort();
    }
    h_trace_t *e = &g_trace[g_ntrace++];
    e->vt_us = vt;
    snprintf(e->task, sizeof e->task, "%s", task);
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->a, sizeof e->a, "%s", a ? a : "");
    e->n = n;
}

void h_trace(const char *kind, const char *a, int64_t n)
{
    pthread_mutex_lock(&G);
    h_trace_locked_ext(g_vt_us, t_self ? t_self->name : "?", kind, a, n);
    pthread_mutex_unlock(&G);
}

size_t h_trace_count(void) { return g_ntrace; }
const h_trace_t *h_trace_at(size_t i) { return &g_trace[i]; }

/* ── ESP_LOG sink ── */

static FILE *g_logf;
static h_log_hook_fn g_log_hook;

void h_log_open(const char *path) { g_logf = fopen(path, "w"); }
void h_log_set_hook(h_log_hook_fn fn) { g_log_hook = fn; }

void h_log(char level, const char *tag, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    uint64_t vt = h_vt_us();
    if (g_logf != NULL) {
        fprintf(g_logf, "[%10.3f] %-12s %c %s: %s\n", (double)vt / 1e6, h_task_name(), level, tag, msg);
    }
    if (g_log_hook != NULL) g_log_hook(level, tag, msg);
}
