#include "svc_engine.h"

#include <string.h>

static void lock(svc_engine_t *e) { e->ports.lock(e->ports.context); }
static void unlock(svc_engine_t *e) { e->ports.unlock(e->ports.context); }
static bool ready(svc_engine_t *e) { return e->ports.boot_ready && e->ports.boot_ready(e->ports.context); }
static int64_t now(svc_engine_t *e) { return e->ports.now_ms(e->ports.context); }
static void reason(svc_record_t *r, const char *text)
{
    /* Reasons are fixed implementation strings, never Wi-Fi/driver input. */
    size_t n = strlen(text);
    if (n >= sizeof r->reason) n = sizeof r->reason - 1;
    memset(r->reason, 0, sizeof r->reason); memcpy(r->reason, text, n);
}
static bool hash(void *context, const void *bytes, size_t n, unsigned char out[32])
{
    svc_engine_t *e = context;
    return e->ports.protocol.sha256 && e->ports.protocol.sha256(e->ports.protocol.context, bytes, n, out);
}
static void metrics(void *context, svc_metrics_t *out)
{
    svc_engine_t *e = context;
    if (e->ports.protocol.metrics) e->ports.protocol.metrics(e->ports.protocol.context, out);
}
static bool status(void *context, svc_wifi_status_t *out)
{
    svc_engine_t *e = context;
    return e->ports.protocol.wifi_status && e->ports.protocol.wifi_status(e->ports.protocol.context, out);
}

typedef struct { svc_engine_t *engine; svc_record_t *record; const char *error; } prepare_t;
static bool prepare(bool intent, const char *old_ssid, const char *old_password, void *context)
{
    prepare_t *p = context;
    svc_engine_t *e = p->engine;
    lock(e);
    svc_record_t current;
    p->error = svc_store_get(e->protocol.store, p->record->id, &current);
    if (!p->error && (!e->worker_owned || e->job.id != current.id || current.kind != SVC_WIFI || current.state != SVC_CONNECTING))
        p->error = "invalid_sequence";
    if (!p->error) {
        if (intent) p->record->state = SVC_PERSISTING;
        else if (!old_ssid || !old_password || !svc_wifi_fingerprint(&e->protocol.ports, old_ssid, old_password, p->record->previous_fingerprint))
            p->error = "storage_error";
        if (!p->error) p->error = svc_store_save(e->protocol.store, p->record);
    }
    unlock(e);
    return !p->error;
}

static void worker(void *context)
{
    svc_engine_t *e = context;
    lock(e);
    svc_record_t r = e->job;
    char password[sizeof e->password];
    memcpy(password, e->password, sizeof password);
    svc_wipe(e->password, sizeof e->password);
    unlock(e);
    bool maintenance = e->ports.maintenance_begin && e->ports.maintenance_begin(e->ports.context);
    if (!maintenance) { r.state = SVC_FAILED; reason(&r, "busy"); }
    else if (r.kind == SVC_SCHEDULE) {
        svc_truth_t applied = SVC_FALSE;
        const char *error = e->ports.schedule_apply ? e->ports.schedule_apply(e->ports.context, &r, &applied) : "busy";
        bool matched = !error && e->ports.schedule_matches && e->ports.schedule_matches(e->ports.context, &r);
        r.applied = applied;
        r.state = matched ? SVC_SUCCEEDED : (!error || !strcmp(error, "activation_unverified")) ? SVC_UNCERTAIN : SVC_FAILED;
        reason(&r, matched ? (applied == SVC_FALSE ? "already_active" : "") : error ? error : "activation_unverified");
    } else {
        svc_trial_result_t trial = {.reason = "wifi_unavailable"};
        prepare_t preparation = {.engine = e, .record = &r};
        if (e->ports.wifi_trial) e->ports.wifi_trial(e->ports.context, r.ssid, password, prepare, &preparation, &trial);
        if (preparation.error) trial.reason = preparation.error;
        bool success = !trial.reason && trial.saved && trial.connected && trial.persist_intent;
        bool intent = trial.persist_intent || r.state == SVC_PERSISTING;
        r.state = success ? SVC_SUCCEEDED : intent ? SVC_UNCERTAIN : SVC_FAILED;
        r.saved = success ? SVC_TRUE : intent ? SVC_UNKNOWN : SVC_FALSE;
        r.connected = intent ? (trial.connected ? SVC_TRUE : SVC_FALSE) : SVC_FALSE;
        r.retrying = trial.retrying; r.restored = trial.restored;
        r.restoration_connected = !intent && trial.restored && trial.connected;
        reason(&r, success ? "" : intent ? "unprovable" : trial.reason ? trial.reason : "wifi_failed");
    }
    svc_wipe(password, sizeof password);
    lock(e);
    /* A callback can never write over a different operation. The slot remains
     * owned until this exact worker ends, even after the progress deadline. */
    svc_record_t current;
    const char *error = svc_store_get(e->protocol.store, r.id, &current);
    if (!error && e->worker_owned && e->job.id == r.id && !svc_terminal(current.state)) {
        r.timed_out = current.timed_out;
        error = svc_store_save(e->protocol.store, &r);
    }
    /* Keep the observed final evidence in the already reserved job slot when
     * persistence fails. Retrying this record is safe. Re-reading RAM Wi-Fi in
     * this same boot cannot prove an NVS commit, so boot reconciliation is NOT
     * used as a substitute for the failed final-result write. */
    e->completion_pending = error != NULL;
    if (e->completion_pending) e->job = r;
    else svc_wipe(&e->job, sizeof e->job);
    if (e->recovery_id == r.id) e->recovery_id = 0;
    e->worker_owned = false;
    unlock(e);
    if (maintenance) e->ports.maintenance_end(e->ports.context);
    svc_wipe(&r, sizeof r);
}

static bool submit(void *context, const svc_record_t *r, const char *password)
{
    svc_engine_t *e = context;
    /* Called with engine lock held by dispatch. Submit only copies/enqueues;
     * the resident worker cannot pass its entry lock until dispatch returns. */
    if (!ready(e) || e->worker_owned || e->completion_pending || !e->ports.submit) return false;
    e->job = *r;
    if (password) memcpy(e->password, password, strlen(password) + 1);
    e->admitted_ms = now(e); e->worker_owned = true;
    if (e->ports.submit(e->ports.context, worker, e)) return true;
    e->worker_owned = false;
    svc_wipe(&e->job, sizeof e->job); svc_wipe(e->password, sizeof e->password);
    return false;
}

void svc_engine_init(svc_engine_t *e, svc_store_t *store, const svc_engine_ports_t *ports)
{
    memset(e, 0, sizeof *e); e->ports = *ports;
    svc_protocol_ports_t protocol = {.sha256 = hash, .submit = submit, .metrics = metrics,
                                      .wifi_status = status, .context = e};
    svc_protocol_init(&e->protocol, store, &protocol);
    svc_record_t active;
    if (!svc_store_active(store, &active)) e->recovery_id = active.id;
    svc_wipe(&active, sizeof active);
}
bool svc_engine_dispatch(svc_engine_t *e, char *body, char *json)
{
    lock(e);
    bool exit = svc_protocol_dispatch(&e->protocol, body, json);
    unlock(e);
    return exit;
}
bool svc_engine_wifi_busy(svc_engine_t *e)
{
    lock(e);
    bool busy = e->worker_owned && e->job.kind == SVC_WIFI;
    unlock(e);
    return busy;
}
void svc_engine_unbind(svc_engine_t *e)
{
    lock(e); svc_protocol_unbind(&e->protocol); unlock(e);
}
void svc_engine_reset_session(svc_engine_t *e)
{
    lock(e); svc_protocol_reset_session(&e->protocol); unlock(e);
}
const char *svc_engine_tick(svc_engine_t *e)
{
    lock(e);
    svc_record_t r;
    const char *error;
    if (e->completion_pending) {
        error = svc_store_save(e->protocol.store, &e->job);
        if (!error) { e->completion_pending = false; svc_wipe(&e->job, sizeof e->job); }
        unlock(e);
        return error;
    }
    error = svc_store_active(e->protocol.store, &r);
    if (error && !strcmp(error, "not_found")) error = NULL;
    else if (!error && e->worker_owned) {
        if (r.kind == SVC_SCHEDULE && r.state == SVC_COMMITTING && !r.timed_out && now(e) - e->admitted_ms >= 120000) {
            r.timed_out = true; reason(&r, "activation_unverified");
            error = svc_store_save(e->protocol.store, &r);
        }
    } else if (!error && ready(e) && r.id == e->recovery_id && r.state != SVC_STAGING) {
        if (r.kind == SVC_SCHEDULE) {
            bool matched = e->ports.schedule_matches && e->ports.schedule_matches(e->ports.context, &r);
            r.state = matched ? SVC_SUCCEEDED : SVC_UNCERTAIN;
            r.applied = SVC_UNKNOWN;
            reason(&r, matched ? "" : "interrupted");
        } else {
            r.saved = SVC_FALSE;
            reason(&r, "interrupted");
            if (r.state == SVC_PERSISTING) {
                unsigned char current[32] = {0};
                bool known = e->ports.wifi_current_fingerprint && e->ports.wifi_current_fingerprint(e->ports.context, current);
                if (known && !memcmp(current, r.fingerprint, sizeof current)) r.saved = SVC_TRUE;
                else if (known && !memcmp(current, r.previous_fingerprint, sizeof current)) r.saved = SVC_FALSE;
                else { r.saved = SVC_UNKNOWN; reason(&r, "unprovable"); }
                svc_wipe(current, sizeof current);
            }
            r.state = SVC_UNCERTAIN; r.connected = SVC_UNKNOWN;
            r.retrying = false; r.restored = false; r.restoration_connected = false;
        }
        error = svc_store_save(e->protocol.store, &r);
        if (!error) e->recovery_id = 0;
    }
    svc_wipe(&r, sizeof r);
    unlock(e);
    return error;
}
