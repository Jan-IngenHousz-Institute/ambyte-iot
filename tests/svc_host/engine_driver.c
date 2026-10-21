/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "svc_engine.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static svc_engine_t engine;
static svc_store_t store;
static bool held, maintenance, boot_ready, reject, maintenance_reject, match, lie;
static int64_t clock_ms;
static unsigned submissions, applications, ends;
static void (*pending)(void *);
static void *pending_arg;
static const char *scenario;
static unsigned char current_fingerprint[32];
static bool fingerprint_known;
static char offline_path[192];
static void disconnect_store(void)
{
    snprintf(offline_path, sizeof offline_path, "%s.offline", store.root);
    assert(rename(store.root, offline_path) == 0);
}
static void lock(void *context) { (void)context; assert(!held); held = true; }
static void unlock(void *context) { (void)context; assert(held); held = false; }
static bool ready(void *context) { (void)context; return boot_ready; }
static int64_t now(void *context) { (void)context; return clock_ms; }
static bool hash(void *context, const void *bytes, size_t n, unsigned char out[32])
{
    (void)context;
    unsigned length = 0;
    return EVP_Digest(bytes, n, out, &length, EVP_sha256(), NULL) == 1 && length == 32;
}
static size_t free_bytes(void *context) { (void)context; return 1024 * 1024; }
void svc_store_fault(const char *point) { (void)point; }
static bool submit(void *context, void (*run)(void *), void *arg)
{
    (void)context; assert(held);
    if (reject) return false;
    ++submissions; pending = run; pending_arg = arg;
    return true;
}
static bool begin_maintenance(void *context)
{
    (void)context; assert(!held && !maintenance);
    if (maintenance_reject) return false;
    maintenance = true; return true;
}
static void end_maintenance(void *context)
{
    (void)context; assert(!held && maintenance); maintenance = false; ++ends;
}
static const char *dispatch(const char *body)
{
    static char json[SVC_JSON_MAX + 1];
    char copy[512]; strcpy(copy, body);
    svc_engine_dispatch(&engine, copy, json);
    for (size_t i = 0; i < strlen(body); ++i) assert(copy[i] == 0);
    return json;
}
static bool matches(void *context, const svc_record_t *r)
{
    (void)context; assert(r->id == 1); return match;
}
static const char *apply(void *context, const svc_record_t *r, svc_truth_t *applied)
{
    (void)context; assert(!held && maintenance && r->state == SVC_COMMITTING);
    ++applications;
    assert(strstr(dispatch("svc.result 0000000000000001"), "committing"));
    assert(strstr(dispatch("svc.abort 0000000000000001"), "commit_started"));
    clock_ms = 120000;
    assert(!svc_engine_tick(&engine));
    assert(strstr(dispatch("svc.result 0000000000000001"), "\"timed_out\":true"));
    assert(strstr(dispatch("svc.result 0000000000000001"), "committing"));
    assert(strstr(dispatch("svc.result 0000000000000001"), "\"reason\":\"activation_unverified\""));
    if (!strcmp(scenario, "apply_error")) return "workbook_mismatch";
    if (!lie) match = true;
    *applied = !strcmp(scenario, "already_active") ? SVC_FALSE : SVC_TRUE;
    if (!strcmp(scenario, "final_save_error")) disconnect_store();
    return NULL;
}
static void trial(void *context, const char *ssid, const char *password,
                    svc_prepare_fn prepare, void *prepare_context, svc_trial_result_t *out)
{
    (void)context; assert(!held && maintenance);
    assert(!strcmp(ssid, "A3Bench") && !strcmp(password, "wrongpass"));
    assert(svc_engine_wifi_busy(&engine));
    assert(strstr(dispatch("svc.result 0000000000000001"), "connecting"));
    assert(prepare(false, "original", "original-password", prepare_context));
    svc_record_t r; assert(!svc_store_get(&store, 1, &r));
    unsigned char expected[32];
    assert(svc_wifi_fingerprint(&engine.protocol.ports, "original", "original-password", expected));
    assert(!memcmp(expected, r.previous_fingerprint, 32));
    if (!strcmp(scenario, "cut_trial")) _exit(86);
    if (!strcmp(scenario, "auth_restore")) {
        out->reason = "wifi_auth_rejected"; out->restored = true; out->connected = true; return;
    }
    assert(prepare(true, NULL, NULL, prepare_context));
    assert(!svc_store_get(&store, 1, &r) && r.state == SVC_PERSISTING);
    if (!strcmp(scenario, "cut_persist")) _exit(86);
    out->persist_intent = true; out->connected = true;
    if (!strcmp(scenario, "save_error")) { out->reason = "wifi_failed"; return; }
    out->reason = NULL; out->saved = true;
    if (!strcmp(scenario, "wifi_final_save_error")) disconnect_store();
}
static bool current(void *context, unsigned char out[32])
{
    (void)context; memcpy(out, current_fingerprint, 32); return fingerprint_known;
}
static void run_pending(void)
{
    assert(pending);
    pending(pending_arg); pending = NULL;
    assert(!engine.worker_owned && !held && !maintenance);
    for (unsigned i = 0; i < sizeof engine.password; ++i) assert(!engine.password[i]);
    if (strstr(scenario, "final_save_error")) {
        assert(engine.completion_pending);
        assert(svc_engine_tick(&engine));
        assert(rename(offline_path, store.root) == 0);
        assert(!svc_engine_tick(&engine));
        assert(!engine.completion_pending);
    }
    for (unsigned i = 0; i < sizeof engine.job; ++i) assert(!((unsigned char *)&engine.job)[i]);
}
int main(int argc, char **argv)
{
    assert(argc == 3); scenario = argv[2];
    assert(!svc_store_open(&store, argv[1], 1, free_bytes, NULL));
    svc_engine_ports_t ports = {
        .protocol = {.sha256 = hash}, .lock = lock, .unlock = unlock,
        .submit = submit, .maintenance_begin = begin_maintenance, .maintenance_end = end_maintenance,
        .boot_ready = ready, .now_ms = now, .schedule_apply = apply, .schedule_matches = matches,
        .wifi_trial = trial, .wifi_current_fingerprint = current,
    };
    svc_engine_init(&engine, &store, &ports);
    dispatch("svc.bind 0000000000000001");
    svc_record_t r;
    if (!strncmp(scenario, "recover_", 8)) {
        assert(!svc_store_get(&store, 1, &r));
        svc_state_t before = r.state;
        assert(!svc_engine_tick(&engine));
        assert(!svc_store_get(&store, 1, &r) && r.state == before); /* boot gate */
        boot_ready = true; match = !strcmp(scenario, "recover_schedule_match");
        if (!strcmp(scenario, "recover_requested")) { fingerprint_known = true; memcpy(current_fingerprint, r.fingerprint, 32); }
        if (!strcmp(scenario, "recover_previous")) { fingerprint_known = true; memcpy(current_fingerprint, r.previous_fingerprint, 32); }
        if (!strcmp(scenario, "recover_other")) { fingerprint_known = true; memset(current_fingerprint, 0x7f, 32); }
        assert(!svc_engine_tick(&engine));
        assert(!svc_store_get(&store, 1, &r));
        assert(!submissions && !applications && !ends);
    } else {
        boot_ready = strcmp(scenario, "not_ready") != 0;
        reject = !strcmp(scenario, "queue_full");
        maintenance_reject = !strcmp(scenario, "maintenance_busy");
        lie = !strcmp(scenario, "false_completion");
        bool wifi = strstr(scenario, "wifi") || !strcmp(scenario, "auth_restore") || !strcmp(scenario, "save_error") || !strncmp(scenario, "cut_", 4);
        if (wifi) dispatch("svc.wifi.join 0000000000000001 QTNCZW5jaA== d3JvbmdwYXNz");
        else {
            dispatch("schedule.begin 0000000000000001 00000003 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad a3 test -");
            dispatch("p 0000000000000001 00000000 352441c2 YWJj");
            dispatch("schedule.commit 0000000000000001");
        }
        if (pending) {
            if (!strcmp(scenario, "queued_schedule")) { puts("queued"); return 0; }
            run_pending();
        }
        assert(!svc_store_get(&store, 1, &r));
        if (!reject && boot_ready) assert(submissions == 1);
        assert(ends == (maintenance_reject || reject || !boot_ready ? 0u : 1u));
    }
    char json[SVC_JSON_MAX + 1]; svc_protocol_result(&engine.protocol, &r, json); puts(json);
}
