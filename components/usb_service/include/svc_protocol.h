#pragma once

#include "svc_store.h"
#include "svc_wire.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_AMBYTE_SVC_BENCH
#include "svc_bench.h"
#endif

typedef struct {
    uint32_t free_heap, largest_block, min_heap;
    uint32_t cli_stack, worker_stack, runner_stack;
    uint32_t dropped_bytes, dropped_calls;
} svc_metrics_t;

typedef struct {
    char configured_ssid[33], trial_ssid[33];
    bool trial, connected, provisioned;
} svc_wifi_status_t;

/* Boundary ports only. submit must copy both arguments before returning, must
 * not run the job inline, and must never retain a pointer into the RX buffer.
 * Callers serialize dispatch and worker store access with the same mutex. No
 * worker is allowed to wait on that mutex while dispatch waits for the worker. */
typedef struct {
    bool (*sha256)(void *context, const void *bytes, size_t length, unsigned char out[32]);
    bool (*submit)(void *context, const svc_record_t *record, const char *password);
    void (*metrics)(void *context, svc_metrics_t *out);
    bool (*wifi_status)(void *context, svc_wifi_status_t *out);
    void *context;
} svc_protocol_ports_t;

typedef struct {
    svc_store_t *store;
    svc_protocol_ports_t ports;
    uint64_t bound_instance;
#if CONFIG_AMBYTE_SVC_BENCH
    svc_bench_t bench;
#endif
} svc_protocol_t;

void svc_protocol_init(svc_protocol_t *protocol, svc_store_t *store,
                       const svc_protocol_ports_t *ports);
void svc_protocol_unbind(svc_protocol_t *protocol);
/* body is a CRC-verified mutable ASCII request without the frame wrapper. It
 * is always wiped, including credentials and token separators, before return.
 * json must have SVC_JSON_MAX+1 bytes. true requests mode exit after emission. */
bool svc_protocol_dispatch(svc_protocol_t *protocol, char *body, char *json);
void svc_protocol_error(const svc_protocol_t *protocol, const char *reason, char *json);
void svc_protocol_result(const svc_protocol_t *protocol, const svc_record_t *record, char *json);
/* Same domain-separated, length-delimited fingerprint for requested and prior
 * credentials. The plaintext packing workspace is wiped on every path. */
bool svc_wifi_fingerprint(const svc_protocol_ports_t *ports, const char *ssid,
                           const char *password, unsigned char out[32]);
