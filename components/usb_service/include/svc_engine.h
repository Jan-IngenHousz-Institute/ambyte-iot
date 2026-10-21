#pragma once

#include "svc_protocol.h"

typedef bool (*svc_prepare_fn)(bool persist_intent, const char *old_ssid,
                                const char *old_password, void *context);
typedef struct {
    const char *reason; /* NULL only for a conclusive saved, owned-GOT_IP success */
    bool saved, connected, retrying, restored, persist_intent;
} svc_trial_result_t;

typedef struct {
    svc_protocol_ports_t protocol; /* submit is provided by the engine */
    void (*lock)(void *context);
    void (*unlock)(void *context);
    bool (*submit)(void *context, void (*run)(void *), void *arg);
    bool (*maintenance_begin)(void *context);
    void (*maintenance_end)(void *context);
    bool (*boot_ready)(void *context);
    int64_t (*now_ms)(void *context);
    /* Runs synchronously on the existing maintenance worker with its global
     * lock already held. Validates before side effects and returns a documented
     * reason on failure; exact activation is checked again by the engine. */
    const char *(*schedule_apply)(void *context, const svc_record_t *record, svc_truth_t *applied);
    bool (*schedule_matches)(void *context, const svc_record_t *record);
    void (*wifi_trial)(void *context, const char *ssid, const char *password,
                        svc_prepare_fn prepare, void *prepare_context, svc_trial_result_t *out);
    bool (*wifi_current_fingerprint)(void *context, unsigned char out[32]);
    void *context;
} svc_engine_ports_t;

typedef struct {
    svc_protocol_t protocol;
    svc_engine_ports_t ports;
    bool worker_owned;
    bool completion_pending;
    uint64_t recovery_id;
    int64_t admitted_ms;
    svc_record_t job;
    char password[64];
} svc_engine_t;

void svc_engine_init(svc_engine_t *engine, svc_store_t *store, const svc_engine_ports_t *ports);
/* All three calls below take the engine mutex. tick also reconciles interrupted
 * operations only after boot_ready, when installed identity and boot Wi-Fi
 * config are authoritative. It never retries an interrupted action. */
bool svc_engine_dispatch(svc_engine_t *engine, char *body, char *json);
const char *svc_engine_tick(svc_engine_t *engine);
bool svc_engine_wifi_busy(svc_engine_t *engine);
void svc_engine_unbind(svc_engine_t *engine);
