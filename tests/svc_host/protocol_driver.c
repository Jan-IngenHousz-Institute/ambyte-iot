/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "svc_protocol.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static svc_record_t queued;
static char secret[64];
static unsigned submissions;
static bool hash(void *context, const void *bytes, size_t n, unsigned char out[32])
{
    (void)context;
    if (getenv("SVC_HASH_FAIL")) return false;
    unsigned length = 0;
    return EVP_Digest(bytes, n, out, &length, EVP_sha256(), NULL) == 1 && length == 32;
}
static bool submit(void *context, const svc_record_t *record, const char *password)
{
    (void)context;
    if (getenv("SVC_REJECT")) return false;
    ++submissions; queued = *record;
    if (password) strcpy(secret, password);
    return true;
}
static void metrics(void *context, svc_metrics_t *out)
{
    (void)context;
    out->free_heap = 128000; out->largest_block = 64000; out->min_heap = 120000;
    out->cli_stack = 6000; out->worker_stack = submissions;
}
static bool status(void *context, svc_wifi_status_t *out)
{
    (void)context;
    strcpy(out->configured_ssid, "original"); strcpy(out->trial_ssid, "trial");
    out->trial = getenv("SVC_TRIAL") != NULL;
    out->connected = true; out->provisioned = true;
    return !getenv("SVC_NO_WIFI");
}
static size_t free_bytes(void *context)
{
    (void)context;
    return getenv("SVC_NO_SPACE") ? 0 : 1024 * 1024;
}
void svc_store_fault(const char *point) { (void)point; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    svc_store_t store;
    const char *error = svc_store_open(&store, argv[1], UINT64_C(0x0123456789abcdef), free_bytes, NULL);
    /* Queries remain possible with a corrupt store, just as on target. */
    assert(!error || !strcmp(error, "corrupt_store"));
    svc_protocol_ports_t ports = {.sha256 = hash, .submit = submit, .metrics = metrics, .wifi_status = status};
    svc_protocol_t protocol;
    svc_protocol_init(&protocol, &store, &ports);
    char line[2048], json[SVC_JSON_MAX + 1], frame[SVC_FRAME_MAX + 1];
    while (fgets(line, sizeof line, stdin)) {
        if (!strcmp(line, "@finish\n")) {
            assert(queued.id);
            queued.state = SVC_SUCCEEDED;
            if (queued.kind == SVC_WIFI) queued.saved = queued.connected = SVC_TRUE;
            else queued.applied = SVC_TRUE;
            assert(!svc_store_save(&store, &queued));
            svc_wipe(secret, sizeof secret); svc_wipe(&queued, sizeof queued);
            continue;
        }
        if (!strcmp(line, "@unbind\n")) { svc_protocol_unbind(&protocol); continue; }
        if (!strncmp(line, "@dispatch ", 10)) {
            line[strcspn(line, "\n")] = 0;
            size_t n = strlen(line + 10);
            svc_protocol_dispatch(&protocol, line + 10, json);
            for (size_t b = 0; b < n; ++b) assert(line[10 + b] == 0);
            assert(svc_frame(frame, sizeof frame, 1, json));
            fputs(frame, stdout); svc_wipe(line, sizeof line); continue;
        }
        svc_rx_t rx = {0};
        for (size_t i = 0; line[i]; ++i) {
            char *body; uint32_t correlation;
            error = svc_rx_byte(&rx, (unsigned char)line[i], 1, &body, &correlation);
            if (error) {
                svc_protocol_error(&protocol, error, json); correlation = 0;
            } else if (body) {
                size_t n = strlen(body);
                svc_protocol_dispatch(&protocol, body, json);
                for (size_t b = 0; b < n; ++b) assert(body[b] == 0);
            } else continue;
            assert(svc_frame(frame, sizeof frame, correlation, json));
            fputs(frame, stdout);
            svc_rx_clear(&rx);
        }
        svc_wipe(line, sizeof line);
    }
    svc_wipe(secret, sizeof secret);
}
