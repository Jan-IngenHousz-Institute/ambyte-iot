#include "svc_protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* All response construction is bounded independently of metadata validation.
 * A broken port or future schema addition fails closed rather than emitting a
 * truncated JSON object which a client could mistake for another response. */
typedef struct { char *bytes; size_t used; bool failed; } json_t;
static void append(json_t *j, const char *format, ...)
{
    if (j->failed) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(j->bytes + j->used, SVC_JSON_MAX + 1 - j->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n > SVC_JSON_MAX - j->used) j->failed = true;
    else j->used += (size_t)n;
}
static void quoted(json_t *j, const char *s)
{
    append(j, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') append(j, "\\%c", *p);
        else if (*p < 0x20) append(j, "\\u%04x", *p);
        else append(j, "%c", *p);
    }
    append(j, "\"");
}
static const char *truth(svc_truth_t value)
{
    return value == SVC_TRUE ? "true" : value == SVC_FALSE ? "false" : "null";
}
static void prefix(const svc_protocol_t *p, json_t *j, bool ok)
{
    append(j, "{\"ok\":%s,\"store\":", ok ? "true" : "false");
    if (p->store->instance) append(j, "\"%016llx\"", (unsigned long long)p->store->instance);
    else append(j, "null");
}
static void finish(const svc_protocol_t *p, json_t *j)
{
    append(j, "}");
    if (j->failed) svc_protocol_error(p, "storage_error", j->bytes);
}
void svc_protocol_error(const svc_protocol_t *p, const char *reason, char *out)
{
    json_t j = {.bytes = out};
    prefix(p, &j, false);
    append(&j, ",\"reason\":"); quoted(&j, reason); append(&j, "}");
}
void svc_protocol_result(const svc_protocol_t *p, const svc_record_t *r, char *out)
{
    json_t j = {.bytes = out};
    prefix(p, &j, true);
    append(&j, ",\"op\":\"%016llx\",\"kind\":\"%s\",\"state\":\"%s\",\"reason\":",
           (unsigned long long)r->id, r->kind == SVC_SCHEDULE ? "schedule" : "wifi", svc_state_name(r->state));
    quoted(&j, r->reason);
    append(&j, ",\"offset\":%lu,\"size\":%lu", (unsigned long)r->offset, (unsigned long)r->size);
    if (r->kind == SVC_SCHEDULE) {
        append(&j, ",\"sha256\":"); quoted(&j, r->sha256);
        append(&j, ",\"workbook\":");
        if (r->workbook[0]) quoted(&j, r->workbook); else append(&j, "null");
        append(&j, ",\"version\":"); quoted(&j, r->version);
        append(&j, ",\"built_fw\":"); quoted(&j, r->built_fw);
        append(&j, ",\"applied\":%s", truth(r->applied));
        if (r->timed_out) append(&j, ",\"timed_out\":true");
    } else {
        append(&j, ",\"ssid\":"); quoted(&j, r->ssid);
        append(&j, ",\"saved\":%s,\"connected\":%s,\"retrying\":%s,\"restored\":%s,\"restoration_connected\":%s",
               truth(r->saved), truth(r->connected), r->retrying ? "true" : "false", r->restored ? "true" : "false",
               r->restoration_connected ? "true" : "false");
    }
    finish(p, &j);
}
static void summary(const svc_protocol_t *p, const svc_record_t *r, bool abort, char *out)
{
    json_t j = {.bytes = out};
    prefix(p, &j, true);
    append(&j, ",\"op\":\"%016llx\",\"state\":\"%s\"", (unsigned long long)r->id, svc_state_name(r->state));
    if (abort) { append(&j, ",\"reason\":"); quoted(&j, r->reason); }
    else append(&j, ",\"offset\":%lu", (unsigned long)r->offset);
    finish(p, &j);
}

/* Domain + NUL, followed by big-endian 16-bit lengths and exact field bytes.
 * There is no concatenation ambiguity (including empty/open passwords), and
 * schedule and credential fingerprints cannot alias across action kinds. */
static bool fingerprint(const svc_protocol_ports_t *ports, const char *domain,
                         const char *const *fields, unsigned count, unsigned char out[32])
{
    unsigned char bytes[256] = {0};
    size_t length = strlen(domain) + 1;
    bool ok = length <= sizeof bytes;
    if (ok) memcpy(bytes, domain, length);
    for (unsigned i = 0; ok && i < count; ++i) {
        size_t n = strlen(fields[i]);
        if (n > UINT16_MAX || length + 2 + n > sizeof bytes) { ok = false; break; }
        bytes[length++] = (unsigned char)(n >> 8); bytes[length++] = (unsigned char)n;
        memcpy(bytes + length, fields[i], n); length += n;
    }
    if (ok) ok = ports->sha256 && ports->sha256(ports->context, bytes, length, out);
    svc_wipe(bytes, sizeof bytes);
    return ok;
}
bool svc_wifi_fingerprint(const svc_protocol_ports_t *ports, const char *ssid,
                           const char *password, unsigned char out[32])
{
    const char *fields[] = {ssid, password};
    return fingerprint(ports, "svc.wifi.v1", fields, 2, out);
}
static bool hex(const char *s, size_t n, uint64_t *value)
{
    return strlen(s) == n && svc_hex(s, n, value);
}
static bool digest(const char *s)
{
    if (strlen(s) != 64) return false;
    for (unsigned i = 0; i < 64; ++i) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
static bool token(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 31) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '+' || c == '-')) return false;
    }
    return true;
}
static bool workbook(const char *s)
{
    if (!strcmp(s, "-")) return true;
    if (strlen(s) != 36) return false;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    }
    return true;
}
static unsigned split(char *body, char *args[8])
{
    unsigned count = 1;
    if (!*body) return 0;
    args[0] = body;
    for (char *c = body; *c; ++c) {
        if (*c == ' ') {
            if (c == body || c[-1] == 0 || !c[1] || count == 8) return 0;
            *c = 0; args[count++] = c + 1;
        } else if ((unsigned char)*c < 0x21 || (unsigned char)*c > 0x7e) return 0;
    }
    return count;
}
void svc_protocol_init(svc_protocol_t *p, svc_store_t *s, const svc_protocol_ports_t *ports)
{
    memset(p, 0, sizeof *p); p->store = s; p->ports = *ports;
}
void svc_protocol_unbind(svc_protocol_t *p)
{
    p->bound_instance = 0;
#if CONFIG_AMBYTE_SVC_BENCH
    svc_wipe(&p->bench, sizeof p->bench);
#endif
}
void svc_protocol_reset_session(svc_protocol_t *p)
{
    svc_protocol_unbind(p);
    p->expected_instance = 0;
}

static const char *queue(svc_protocol_t *p, svc_record_t *r, const char *password)
{
    if (p->ports.submit && p->ports.submit(p->ports.context, r, password)) return NULL;
    r->state = SVC_FAILED;
    strcpy(r->reason, "busy");
    return svc_store_save(p->store, r);
}

static const char *binding_error(const svc_protocol_t *p)
{
    if (p->expected_instance && p->expected_instance != p->store->instance) return "store_changed";
    return p->bound_instance == p->store->instance && p->bound_instance ? NULL : "not_bound";
}

bool svc_protocol_dispatch(svc_protocol_t *p, char *body, char *out)
{
    size_t length = strlen(body);
    char *a[8] = {0};
    unsigned n = split(body, a);
    const char *error = NULL;
    bool exit = false;
    svc_record_t r = {0};
    unsigned char decoded[145] = {0};
    char password[64] = {0};
    uint64_t id = 0, offset = 0, crc = 0;
    json_t j = {.bytes = out};
    if (!n) { error = "invalid_args"; goto done; }
    if (!strcmp(a[0], "svc.version")) {
        if (n != 1) { error = "invalid_args"; goto done; }
        prefix(p, &j, true);
        append(&j, ",\"service\":1,\"actions\":[\"svc.version\",\"svc.bind\",\"svc.result\",\"svc.stats\",\"schedule.begin\",\"p\",\"schedule.commit\",\"svc.abort\",\"svc.wifi.join\",\"svc.wifi.status\",\"svc.exit\",\"svc.active\""
#if CONFIG_AMBYTE_SVC_BENCH
               ",\"bench.begin\",\"bench.read\",\"bench.end\",\"bench.abort\""
#endif
               "],\"max_line\":248,\"chunk\":144,\"result_slots\":8,\"highest_id\":\"%016llx\"", (unsigned long long)p->store->highest);
        finish(p, &j); goto done;
    }
    if (!strcmp(a[0], "svc.bind")) {
        if (n != 2 || !hex(a[1], 16, &id) || !id) { error = "invalid_args"; goto done; }
        svc_protocol_unbind(p);
        p->expected_instance = id;
        if (id != p->store->instance) { error = "store_changed"; goto done; }
        p->bound_instance = id;
        prefix(p, &j, true); append(&j, ",\"state\":\"bound\""); finish(p, &j); goto done;
    }
    if (!strcmp(a[0], "svc.exit")) {
        if (n != 1) { error = "invalid_args"; goto done; }
        svc_protocol_reset_session(p);
        prefix(p, &j, true); append(&j, ",\"state\":\"exit\""); finish(p, &j); exit = true; goto done;
    }
    if (!strcmp(a[0], "svc.active")) {
        if (n != 1) { error = "invalid_args"; goto done; }
        error = svc_store_active(p->store, &r);
        if (error && strcmp(error, "not_found")) goto done;
        prefix(p, &j, true); append(&j, ",\"active\":");
        if (error) append(&j, "null");
        else append(&j, "{\"op\":\"%016llx\",\"kind\":\"%s\",\"state\":\"%s\"}",
                    (unsigned long long)r.id, r.kind == SVC_SCHEDULE ? "schedule" : "wifi", svc_state_name(r.state));
        error = NULL; finish(p, &j); goto done;
    }
    if (!strcmp(a[0], "svc.stats")) {
        if (n != 1) { error = "invalid_args"; goto done; }
        svc_metrics_t metrics = {0}; size_t logical = 0; unsigned records = 0;
        if ((error = svc_store_account(p->store, &logical, &records))) goto done;
        if (p->ports.metrics) p->ports.metrics(p->ports.context, &metrics);
        prefix(p, &j, true);
        append(&j, ",\"free_heap\":%lu,\"largest_block\":%lu,\"min_heap\":%lu,\"cli_stack\":%lu,\"worker_stack\":%lu,\"runner_stack\":%lu,\"dropped_bytes\":%lu,\"dropped_calls\":%lu,\"logical_bytes\":%lu,\"records\":%u,\"log_buffer_bytes\":0",
               (unsigned long)metrics.free_heap, (unsigned long)metrics.largest_block, (unsigned long)metrics.min_heap,
               (unsigned long)metrics.cli_stack, (unsigned long)metrics.worker_stack, (unsigned long)metrics.runner_stack,
               (unsigned long)metrics.dropped_bytes, (unsigned long)metrics.dropped_calls, (unsigned long)logical, records);
        append(&j, ",\"min_free_heap\":%lu,\"min_largest_block\":%lu,\"samples\":%lu,\"fs_total\":%lu,\"fs_used\":%lu",
               (unsigned long)metrics.min_free_heap, (unsigned long)metrics.min_largest_block,
               (unsigned long)metrics.samples, (unsigned long)metrics.fs_total, (unsigned long)metrics.fs_used);
        finish(p, &j); goto done;
    }
    if (!strcmp(a[0], "svc.wifi.status")) {
        if (n != 1) { error = "invalid_args"; goto done; }
        svc_wifi_status_t status = {0};
        if (!p->ports.wifi_status || !p->ports.wifi_status(p->ports.context, &status)) { error = "wifi_unavailable"; goto done; }
        prefix(p, &j, true); append(&j, ",\"configured_ssid\":"); quoted(&j, status.configured_ssid);
        append(&j, ",\"trial_ssid\":"); if (status.trial) quoted(&j, status.trial_ssid); else append(&j, "null");
        append(&j, ",\"trial\":%s,\"connected\":%s,\"provisioned\":%s", status.trial ? "true" : "false", status.connected ? "true" : "false", status.provisioned ? "true" : "false");
        finish(p, &j); goto done;
    }
    bool begin = !strcmp(a[0], "schedule.begin"), put = !strcmp(a[0], "p");
    bool join = !strcmp(a[0], "svc.wifi.join"), commit = !strcmp(a[0], "schedule.commit");
    bool abort = !strcmp(a[0], "svc.abort"), result = !strcmp(a[0], "svc.result");
#if CONFIG_AMBYTE_SVC_BENCH
    if (!strncmp(a[0], "bench.", 6) || (put && n >= 2 && !strcmp(a[1], "0000000000000000"))) {
        error = binding_error(p);
        if (!error) error = svc_bench_dispatch(&p->bench, p->store, a, n, out);
        goto done;
    }
    if (p->bench.active && (begin || put || join || commit || abort)) { error = "busy"; goto done; }
#endif
    if (!begin && !put && !join && !commit && !abort && !result) { error = "unsupported"; goto done; }
    unsigned wanted = begin ? 7 : put ? 5 : join ? 4 : 2;
    if (n != wanted || !hex(a[1], 16, &id) || !id) { error = "invalid_args"; goto done; }
    if ((error = binding_error(p))) goto done;
    if (begin) {
        if (!hex(a[2], 8, &offset) || !offset || !digest(a[3]) || !token(a[4]) || !token(a[5]) || !workbook(a[6])) { error = "invalid_args"; goto done; }
        if (offset > SVC_DATA_MAX) { error = "size_limit"; goto done; }
        r.id = id; r.size = (uint32_t)offset; r.kind = SVC_SCHEDULE; r.state = SVC_STAGING;
        strcpy(r.sha256, a[3]); strcpy(r.version, a[4]); strcpy(r.built_fw, a[5]);
        if (strcmp(a[6], "-")) strcpy(r.workbook, a[6]);
        const char *fields[] = {a[2], a[3], a[4], a[5], a[6]};
        if (!fingerprint(&p->ports, "svc.schedule.v1", fields, 5, r.fingerprint)) { error = "storage_error"; goto done; }
        bool reused;
        if (!(error = svc_store_admit(p->store, &r, &reused))) summary(p, &r, false, out);
        goto done;
    }
    if (join) {
        int size = svc_base64_decode(a[2], decoded, sizeof decoded - 1);
        if (size < 1 || size > 31 || memchr(decoded, 0, (size_t)size) || !svc_utf8(decoded, (size_t)size)) { error = "invalid_args"; goto done; }
        memcpy(r.ssid, decoded, (size_t)size);
        svc_wipe(decoded, sizeof decoded);
        size = !strcmp(a[3], "-") ? 0 : svc_base64_decode(a[3], decoded, sizeof decoded);
        if (size < 0 || size > 63) { error = "invalid_args"; goto done; }
        for (int i = 0; i < size; ++i) if (decoded[i] < 0x20 || decoded[i] > 0x7e) { error = "invalid_args"; goto done; }
        /* esp_wifi accepts open networks and personal PSKs only. Reject a short
         * key before admission rather than letting a driver truncate it. */
        if (size && size < 8) { error = "wifi_invalid_credentials"; goto done; }
        memcpy(password, decoded, (size_t)size);
        r.id = id; r.kind = SVC_WIFI; r.state = SVC_CONNECTING; r.connected = SVC_UNKNOWN;
        if (!svc_wifi_fingerprint(&p->ports, r.ssid, password, r.fingerprint)) { error = "storage_error"; goto done; }
        bool reused;
        error = svc_store_admit(p->store, &r, &reused);
        if (!error && !reused) error = queue(p, &r, password);
        if (!error) svc_protocol_result(p, &r, out);
        goto done;
    }
    if ((error = svc_store_get(p->store, id, &r))) goto done;
    if (result) { svc_protocol_result(p, &r, out); goto done; }
    if (put) {
        if (!hex(a[2], 8, &offset) || !hex(a[3], 8, &crc)) { error = "invalid_args"; goto done; }
        int size = svc_base64_decode(a[4], decoded, sizeof decoded);
        if (size == -2) { error = "size_limit"; goto done; }
        if (size < 1) { error = "invalid_args"; goto done; }
        error = svc_store_put(p->store, &r, (uint32_t)offset, (uint32_t)crc, decoded, (size_t)size);
        if (!error) summary(p, &r, false, out);
        goto done;
    }
    if (abort) {
        if (r.kind != SVC_SCHEDULE && !svc_terminal(r.state)) error = "invalid_sequence";
        else error = svc_store_abort(p->store, &r);
        if (!error) summary(p, &r, true, out);
        goto done;
    }
    if (r.state == SVC_ABORTED) { error = "operation_complete"; goto done; }
    if (r.state == SVC_COMMITTING || svc_terminal(r.state)) { svc_protocol_result(p, &r, out); goto done; }
    if (r.kind != SVC_SCHEDULE) { error = "invalid_sequence"; goto done; }
    if (r.state != SVC_STAGING) { error = "invalid_sequence"; goto done; }
    if (r.offset != r.size) { error = "incomplete"; goto done; }
    r.state = SVC_COMMITTING;
    if ((error = svc_store_save(p->store, &r))) goto done;
    error = queue(p, &r, NULL);
    if (!error) svc_protocol_result(p, &r, out);
done:
    if (error) svc_protocol_error(p, error, out);
    svc_wipe(password, sizeof password); svc_wipe(decoded, sizeof decoded);
    svc_wipe(&r, sizeof r); svc_wipe(body, length);
    return exit;
}
