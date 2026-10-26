#pragma once

#include "svc_engine.h"

typedef struct {
    bool (*enter)(void *context);
    void (*leave)(void *context);
    int (*read)(void *context, void *bytes, size_t capacity, unsigned timeout_ms);
    bool (*write)(void *context, const void *bytes, size_t length);
    bool (*overrun)(void *context);
    int64_t (*now_ms)(void *context);
    void (*sample)(void *context);
    /* Positive blocking delay, outside engine/stdio locks. Ready RX/TX and
     * synchronous flash work otherwise need not give the CPU's idle task a
     * turn. The target adapter must request a positive tick-count delay;
     * vTaskDelay(1) can last less than one complete tick period. This hook
     * does not bound a single backend call or other tasks' CPU demand. */
    void (*cooperate)(void *context);
    void *context;
} svc_session_ports_t;

/* Runs on the existing CLI task. enter/leave gate the normal USB writer; all
 * service writes are complete framed lines. No request survives this call's
 * stack, and disconnect/exit never cancels an admitted worker or staged data. */
bool svc_session_run(svc_engine_t *engine, const svc_session_ports_t *ports);
