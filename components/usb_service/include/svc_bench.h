#pragma once

#include "svc_store.h"
#include "svc_wire.h"

/* Diagnostic only: no filesystem path, payload retention or worker port. It
 * owns no durable ID. ID zero on the ordinary p wire shape is reserved for
 * this explicitly admitted volatile sink, and remains invalid in production. */
typedef struct {
    bool active, transmit;
    uint32_t size, offset, crc;
    uint16_t chunk;
} svc_bench_t;

const char *svc_bench_dispatch(svc_bench_t *bench, svc_store_t *store,
                                char *const args[], unsigned count, char *json);
