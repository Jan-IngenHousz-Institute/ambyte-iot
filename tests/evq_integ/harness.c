/*
 * harness.c — evq runtime-integration harness (contract group I, §2.2).
 *
 * Compiles against the REAL device_commands.c, event_log.c, evq_index.c,
 * evq_render.c, payload_v3.c, telemetry_publish.c, envelope_provenance.c,
 * payload_scalar.c, payload_gzip.c, ambit_announcement.c and cJSON.c, and
 * #includes the REAL sync_runner.c below so its static sync_runner_drain() and
 * sync_runner_wd_should_reboot() are reachable. Production tasks created by
 * sync_runner_start()/event_log_sd_keeper_start() run as shim threads on the
 * virtual clock (rtos_shim.c).
 *
 * The composition root mirrors main/app_main.c: the persistence ports are the
 * event_log getters (behind pass-through counters), the messaging ports are a
 * broker simulation whose PUBACK/refusal/disconnect reach device_commands only
 * through the handlers device_commands registered via set_publish_ack_handler
 * / set_disconnect_handler — the same functions the esp-mqtt task calls in
 * production (mqtt_client.c MQTT_EVENT_PUBLISHED: reason >= 0x80 →
 * ESP_ERR_NOT_ALLOWED; MQTT_EVENT_DISCONNECTED → disconnect handler), and on
 * connect the broker calls sync_runner_notify() exactly as app_on_mqtt_connect.
 *
 * Output: KEY=VALUE evidence lines and SNAPSHOT_JSON=/WORST_*= lines on stdout;
 * ESP_LOG output in ./esp.log; manifests (accepted.jsonl, accepted_payloads.txt,
 * published.jsonl, envelopes.txt) in the CWD for the independent Python oracle.
 * Exit status 0 only when every CHECK held.
 */
#include "device_commands.h"
#include "event_log.h"
#include "evq_index.h"
#include "payload_v3.h"
#include "sd_card.h"
#include "shim_api.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* The production drain + watchdog predicate, as one translation unit. */
#include "sync_runner.c"

void h_set_clock_source(const char *s);

/* Tuning constants: the build passes every one of them with -D to every TU
 * (production values unless the row needs a smaller store), so the harness
 * sees exactly what event_log.c / device_commands.c were compiled with. */
#if !defined(EVLOG_ROTATE_BYTES) || !defined(EVLOG_MIN_FREE_BYTES) || !defined(EVLOG_ARCHIVE_EVERY_N) || \
    !defined(EVQ_PRESSURE_PCT) || !defined(EVQ_RECLAIM_PCT) || !defined(EVQ_SD_RESERVE_BYTES) || \
    !defined(EVQ_KEEPER_PERIOD_MS) || !defined(EVQ_INDEX_CAP)
#error "build must pass the evq tuning constants with -D"
#endif
#ifndef AMBYTE_PUBLISH_MAX_BYTES
#define AMBYTE_PUBLISH_MAX_BYTES (EVLOG_RECORD_CAP_NORMAL + 4096U)   /* device_commands.c default */
#endif

#define TEST_PRIO    10   /* the producer side runs at sched_runner's priority */
#define BROKER_PRIO  5    /* esp-mqtt task priority */
#define SENSOR_PRIO  10
#define TOPIC        "harness/evq-integ/exp/ambyte/1/AMBYTE_240AC4E0517A"
#define CID_A        0xA11CE001u
#define CID_B        0xB0B0B002u

/* ════════════════════════ checks + evidence ═════════════════════════════ */

static int g_failures;
#define EV(key, ...) do { printf("%s=", key); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("CHECK_FAIL=%s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf(" [%s]\n", #cond); fflush(stdout); } } while (0)

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

/* ════════════════════════ log hook (observability) ══════════════════════ */

static bool     g_gate_open;             /* debounced power gate, from its own log line */
static bool     g_power_port;            /* read_power wired → the gate exists */
static uint64_t g_log_sr_wait, g_log_el_wait, g_log_errors;
static char     g_last_oversize_msg[256];
static uint32_t g_oversize_from_msg;

static void log_hook(char level, const char *tag, const char *msg)
{
    if (level == 'E') g_log_errors++;
    if (strcmp(tag, "sync_runner") == 0) {
        if (strncmp(msg, "heap:", 5) == 0) h_trace("drain_enter", "", 0);
        else if (strncmp(msg, "delivery waiting on unreadable SD backlog", 41) == 0) g_log_sr_wait++;
        const char *o = strstr(msg, "publish-cap skip id=");
        if (o != NULL) {
            snprintf(g_last_oversize_msg, sizeof g_last_oversize_msg, "%s", msg);
            const char *k = strstr(msg, "oversize_skipped=");
            if (k != NULL) g_oversize_from_msg = (uint32_t)strtoul(k + 17, NULL, 10);
        }
    } else if (strcmp(tag, "event_log") == 0) {
        if (strncmp(msg, "delivery waits at", 17) == 0) g_log_el_wait++;
    } else if (strcmp(tag, "dev_cmd") == 0) {
        if (strncmp(msg, "publish gate OPEN", 17) == 0) { g_gate_open = true; h_trace("gate", "open", 1); }
        else if (strncmp(msg, "publish gate CLOSED", 19) == 0) { g_gate_open = false; h_trace("gate", "closed", 0); }
    }
}

/* ════════════════════════ records ═══════════════════════════════════════ */

typedef struct {
    int64_t id, start_ms, end_ms;
    bool    v3;
    char   *cmd, *meta, *payload;
    size_t  line_len;
} rec_t;

static bool    g_trace_stores;
static rec_t  *g_acc;
static size_t  g_nacc, g_capacc;
static int64_t g_refused[8];            /* by class: 0 NO_MEM 1 FAIL 2 INVALID_SIZE 3 other */
static uint64_t g_stored_bytes;
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static const char *g_scenario = "?";
static unsigned g_seed = 7;
static int g_size_profile;              /* 0 = H-7 mix, 1 = fixed g_fixed_size, 2 = small */
static size_t g_fixed_size = 4000;

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}

static size_t pick_line_size(void)
{
    if (g_size_profile == 1) return g_fixed_size;
    if (g_size_profile == 2) return 200 + rnd() % 400;
    uint32_t r = rnd() % 100;
    if (r < 70) return 200 + rnd() % (2048 - 200);
    if (r < 95) return 2048 + rnd() % (16384 - 2048);
    return 61440 + rnd() % (65191 - 61440);
}

static int64_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static char *xstrdup(const char *s) { char *d = strdup(s); if (!d) abort(); return d; }

/* Build + store one synthetic record through the production producer path
 * (cmd_next_measure_id + cmd_store_event, which also fires the sync notifier). */
static esp_err_t produce(void)
{
    static const char alnum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t target = pick_line_size();
    int64_t id = 0;
    cmd_result_t nr = cmd_next_measure_id(&id);
    if (nr.status != ESP_OK) return nr.status;
    unsigned idx = (unsigned)(g_nacc + (size_t)g_refused[0] + (size_t)g_refused[1] + (size_t)g_refused[2] + (size_t)g_refused[3]);
    bool v3 = (idx % 5) == 4;

    char cmd[600];
    size_t cl = (size_t)snprintf(cmd, sizeof cmd, "arrun 1,0,0,0,0,4,0,4,0,1");
    size_t extra = rnd() % 500;
    while (cl + 4 < sizeof cmd && extra >= 4) { memcpy(cmd + cl, ",1,0", 4); cl += 4; extra -= 4; }
    cmd[cl] = '\0';
    char meta[48] = "";
    if (!v3 && (rnd() & 1)) snprintf(meta, sizeof meta, "{\"m\":%u,\"synthetic_test\":true}", idx);

    char head[256];
    int hn = v3 ? snprintf(head, sizeof head,
                           "{\"schema\":\"ambit.trace/3\",\"measure_id\":%lld,\"synthetic_test\":true,"
                           "\"scenario\":\"%s\",\"seed\":%u,\"i\":%u,\"blob\":\"",
                           (long long)id, g_scenario, g_seed, idx)
                : snprintf(head, sizeof head,
                           "{\"synthetic_test\":true,\"scenario\":\"%s\",\"seed\":%u,\"i\":%u,\"blob\":\"",
                           g_scenario, g_seed, idx);
    size_t overhead = 20 + 7 + 6 + 12 + cl + 30 + strlen(meta) + 9;
    size_t plen = target > overhead + (size_t)hn + 2 ? target - overhead : (size_t)hn + 32;
    char *payload = malloc(plen + 1);
    if (payload == NULL) abort();
    memcpy(payload, head, (size_t)hn);
    size_t p = (size_t)hn;
    while (p + 2 < plen) payload[p++] = alnum[rnd() % (sizeof alnum - 1)];
    payload[p++] = '"'; payload[p++] = '}'; payload[p] = '\0';

    int64_t start = wall_ms();
    int64_t end = start + 850 + (int64_t)(rnd() % 3000);
    measurement_event_desc_t d = {
        .measure_id = id, .channel = "uart_1", .device = "ambit", .tag = MEASUREMENT_TAG_MEASUREMENT,
        .cmd_raw = cmd, .start_ms = start, .end_ms = end,
        .metadata_json = meta[0] ? meta : NULL, .payload_json = payload,
    };
    if (g_trace_stores) h_trace("store", "enter", id);
    cmd_result_t r = cmd_store_event(&d);
    if (g_trace_stores) h_trace("store_ret", r.status == ESP_OK ? "ok" : esp_err_to_name(r.status), id);
    if (r.status != ESP_OK) {
        int cls = r.status == ESP_ERR_NO_MEM ? 0 : r.status == ESP_FAIL ? 1 : r.status == ESP_ERR_INVALID_SIZE ? 2 : 3;
        g_refused[cls]++;
        free(payload);
        return r.status;
    }
    if (g_nacc == g_capacc) {
        g_capacc = g_capacc ? g_capacc * 2 : 1024;
        g_acc = realloc(g_acc, g_capacc * sizeof *g_acc);
        if (g_acc == NULL) abort();
    }
    rec_t *a = &g_acc[g_nacc++];
    a->id = id; a->start_ms = start; a->end_ms = end; a->v3 = v3;
    a->cmd = xstrdup(cmd); a->meta = meta[0] ? xstrdup(meta) : NULL; a->payload = payload;
    a->line_len = strlen(payload) + cl + strlen(meta) + 60;
    g_stored_bytes += a->line_len;
    return ESP_OK;
}

/* ════════════════════════ broker simulation ═════════════════════════════ */

enum { OUT_PENDING = 0, OUT_OK, OUT_REFUSED, OUT_LOST };

typedef struct {
    uint64_t vt_us;
    int      msg_id;           /* 0 = fire-and-forget (direct TELEMETRY) */
    int64_t  mid;
    size_t   len;
    char    *env;
    int      outcome;
    int      out_count;        /* outstanding after this publish, broker view */
    size_t   out_bytes;
} pub_t;

static pub_t *g_pub;
static size_t g_npub, g_cappub;

typedef struct { int msg_id; uint64_t due; esp_err_t st; size_t pub; } pend_t;

static struct {
    bool connected;
    int next_msg;
    message_publish_ack_fn ack; void *ack_ctx;
    message_disconnect_fn disc; void *disc_ctx;
    uint64_t latency_us;
    unsigned early_every, early_count, early_acks;
    uint64_t refuse_until_us;
    bool hold_acks;
    pend_t pend[64]; int npend;
    uint32_t refused_total; int last_reason; uint32_t connects;
    uint64_t connected_since;
    volatile bool cmd_disconnect, cmd_connect;
    TaskHandle_t task;
    int max_out_count; size_t max_out_bytes; int window_violations;
    uint64_t publishes_while_disconnected;
} B;

static void outstanding(int *count, size_t *bytes)
{
    int c = 0; size_t b = 0;
    for (int i = 0; i < B.npend; i++) { c++; b += g_pub[B.pend[i].pub].len; }
    *count = c; *bytes = b;
}

static int64_t env_measure_id(const char *env)
{
    const char *k = strstr(env, "\"measure_id\":");
    return k ? strtoll(k + 13, NULL, 10) : -1;
}

static esp_err_t broker_publish(const char *topic, const char *payload, size_t len, int *out_msg_id)
{
    (void)topic;
    if (!B.connected) { B.publishes_while_disconnected++; return ESP_ERR_INVALID_STATE; }
    if (g_npub == g_cappub) {
        g_cappub = g_cappub ? g_cappub * 2 : 1024;
        g_pub = realloc(g_pub, g_cappub * sizeof *g_pub);
        if (g_pub == NULL) abort();
    }
    size_t pi = g_npub++;
    pub_t *p = &g_pub[pi];
    memset(p, 0, sizeof *p);
    p->vt_us = h_vt_us();
    p->len = len;
    p->env = malloc(len + 1);
    if (p->env == NULL) abort();
    memcpy(p->env, payload, len);
    p->env[len] = '\0';
    p->mid = env_measure_id(p->env);
    if (out_msg_id == NULL) { p->outcome = OUT_OK; return ESP_OK; }   /* direct TELEMETRY */

    int cnt; size_t bytes;
    outstanding(&cnt, &bytes);
    if (cnt + 1 > (int)PUBLISH_WINDOW_SLOTS || (cnt > 0 && bytes + len > PUBLISH_WINDOW_BYTES)) B.window_violations++;
    int msg = ++B.next_msg;
    p->msg_id = msg;
    *out_msg_id = msg;
    esp_err_t st = (h_vt_us() < B.refuse_until_us) ? ESP_ERR_NOT_ALLOWED : ESP_OK;
    if (B.early_every != 0 && !B.hold_acks && (++B.early_count % B.early_every) == 0) {
        /* Sub-ms PUBACK on "the other core": the handler runs before publish()
         * returns, exercising device_commands' early-ACK park. */
        p->out_count = cnt + 1; p->out_bytes = bytes + len;
        B.early_acks++;
        p->outcome = st == ESP_OK ? OUT_OK : OUT_REFUSED;
        if (st != ESP_OK) { B.refused_total++; B.last_reason = 0x87; }
        B.ack(msg, st, B.ack_ctx);
        return ESP_OK;
    }
    if (B.npend >= 64) abort();
    B.pend[B.npend++] = (pend_t){ .msg_id = msg, .due = h_vt_us() + B.latency_us, .st = st, .pub = pi };
    p->out_count = cnt + 1; p->out_bytes = bytes + len;
    if (p->out_count > B.max_out_count) B.max_out_count = p->out_count;
    if (p->out_bytes > B.max_out_bytes) B.max_out_bytes = p->out_bytes;
    xTaskNotifyGive(B.task);
    return ESP_OK;
}

static void broker_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t now = h_vt_us(), due = UINT64_MAX;
        if (!B.hold_acks) for (int i = 0; i < B.npend; i++) if (B.pend[i].due < due) due = B.pend[i].due;
        TickType_t ticks = portMAX_DELAY;
        if (due != UINT64_MAX) {
            uint64_t tick_us = 1000000ULL / configTICK_RATE_HZ;
            ticks = due <= now ? 0 : (TickType_t)((due - now + tick_us - 1) / tick_us);
        }
        if (ticks != 0) (void)ulTaskNotifyTake(pdTRUE, ticks);
        if (B.cmd_disconnect) {
            B.cmd_disconnect = false;
            B.connected = false;
            for (int i = 0; i < B.npend; i++) g_pub[B.pend[i].pub].outcome = OUT_LOST;
            B.npend = 0;
            h_trace("mqtt", "disconnected", 0);
            if (B.disc != NULL) B.disc(B.disc_ctx);      /* MQTT_EVENT_DISCONNECTED */
        }
        if (B.cmd_connect) {
            B.cmd_connect = false;
            B.connected = true;
            B.connects++;
            B.connected_since = h_vt_us();
            h_trace("mqtt", "connected", 0);
            sync_runner_notify();                        /* app_on_mqtt_connect */
        }
        now = h_vt_us();
        while (!B.hold_acks) {
            int k = -1;
            for (int i = 0; i < B.npend; i++) if (B.pend[i].due <= now && (k < 0 || B.pend[i].due < B.pend[k].due)) k = i;
            if (k < 0) break;
            pend_t pe = B.pend[k];
            memmove(&B.pend[k], &B.pend[k + 1], (size_t)(B.npend - k - 1) * sizeof B.pend[0]);
            B.npend--;
            g_pub[pe.pub].outcome = pe.st == ESP_OK ? OUT_OK : OUT_REFUSED;
            if (pe.st != ESP_OK) { B.refused_total++; B.last_reason = 0x87; }
            B.ack(pe.msg_id, pe.st, B.ack_ctx);          /* MQTT_EVENT_PUBLISHED */
        }
    }
}

static void broker_connect(void)    { B.cmd_connect = true; xTaskNotifyGive(B.task); }
static void broker_disconnect(void) { B.cmd_disconnect = true; xTaskNotifyGive(B.task); }

static bool m_is_connected(void) { return B.connected; }
static uint32_t m_err_disc(uint32_t w) { (void)w; return 0; }
static char g_disc_reason[32] = "mqtt:disconnect";
static void m_conn_stats(uint32_t *c, int64_t *age, char *reason, size_t cap)
{
    if (c) *c = B.connects;
    if (age) *age = B.connected ? (int64_t)((h_vt_us() - B.connected_since) / 1000000ULL) : -1;
    if (reason && cap) snprintf(reason, cap, "%s", g_disc_reason);
}
static esp_err_t m_set_ack(message_publish_ack_fn fn, void *ctx) { B.ack = fn; B.ack_ctx = ctx; return ESP_OK; }
static esp_err_t m_set_disc(message_disconnect_fn fn, void *ctx) { B.disc = fn; B.disc_ctx = ctx; return ESP_OK; }
static void m_refusal_stats(uint32_t *total, int *reason, int64_t *ms)
{
    if (total) *total = B.refused_total;
    if (reason) *reason = B.last_reason;
    if (ms) *ms = -1;
}

/* ════════════════════════ persistence ports (pass-through + counters) ═══ */

static uint64_t g_claims, g_claims_ok, g_claims_nf, g_claims_nfin, g_claims_other, g_claims_gate_closed;
static uint64_t g_marks_synced, g_marks_pending, g_quarantines;
static bool     g_trace_claims;

static bool gate_closed_now(void)
{
    return (g_power_port && !g_gate_open) || device_commands_publish_hold_active();
}

static esp_err_t p_claim(measurement_event_t *out)
{
    bool closed = gate_closed_now();
    esp_err_t r = event_log_claim_next_event(out);
    g_claims++;
    if (r == ESP_OK) g_claims_ok++;
    else if (r == ESP_ERR_NOT_FOUND) g_claims_nf++;
    else if (r == ESP_ERR_NOT_FINISHED) g_claims_nfin++;
    else g_claims_other++;
    if (closed) g_claims_gate_closed++;
    if (g_trace_claims) h_trace("claim", r == ESP_OK ? "ok" : esp_err_to_name(r), r == ESP_OK ? out->measure_id : r);
    return r;
}
static esp_err_t p_synced(int64_t id)  { g_marks_synced++;  return event_log_mark_event_synced(id); }
static esp_err_t p_pending(int64_t id) { g_marks_pending++; return event_log_mark_event_pending(id); }
static esp_err_t p_quarantine(int64_t id) { g_quarantines++; return event_log_quarantine_event(id); }

/* app_main.c app_sd_health, same composition. */
static esp_err_t p_sd_health(bool *io_lost, uint64_t *free_bytes, int64_t *skipped, int64_t *dropped, int64_t *last_acked)
{
    if (io_lost) *io_lost = sdcard_io_lost();
    if (free_bytes) { *free_bytes = 0; (void)event_log_free_bytes(free_bytes); }
    evlog_health_t h;
    if (event_log_health(&h) != ESP_OK) return ESP_FAIL;
    if (skipped) *skipped = h.skipped;
    if (dropped) *dropped = h.dropped;
    if (last_acked) *last_acked = h.last_acked_id;
    return ESP_OK;
}

/* Sensing/power/identity ports (knobs). */
static bool g_input_present;
static power_reading_t g_power = { .battery_mv = 4100, .system_mv = 4000, .input_mv = 5000,
                                   .charge_ma = 120, .input_ma = 300, .charge_status = 2 };
static esp_err_t p_read_power(power_reading_t *out) { *out = g_power; out->input_present = g_input_present; return ESP_OK; }
static float g_env[3] = { 21.5f, 48.25f, 101325.0f };
static esp_err_t p_read_env(measurement_t *m) { m->temperature_c = g_env[0]; m->humidity_percent = g_env[1]; m->pressure_pa = g_env[2]; return ESP_OK; }
static script_identity_t g_script = { .sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
                                      .version = "schedule-v0.0.0-test", .built_against_fw = "0.0.0",
                                      .installed_on_fw = "0.0.0", .release_metadata_verified = true };
static esp_err_t p_script(script_identity_t *out) { *out = g_script; return ESP_OK; }
static char g_wd_reason_override[32];
static bool g_wd_reason_forced;
static esp_err_t p_wd_reason(char *out, size_t cap)
{
    if (g_wd_reason_forced) { snprintf(out, cap, "%s", g_wd_reason_override); return ESP_OK; }
    return sync_runner_get_last_wd_reboot_reason(out, cap);   /* app_main wiring */
}
static uint32_t g_hold_ms;
static esp_err_t p_uart_text(uint8_t ch, const char *cmd, const char *term, char *out, size_t cap, size_t *len, uint32_t timeout)
{
    (void)ch; (void)cmd; (void)term; (void)timeout;
    vTaskDelay(pdMS_TO_TICKS(g_hold_ms));    /* the raw sensor transaction on the wire */
    snprintf(out, cap, "OK");
    *len = 2;
    return ESP_OK;
}

/* ════════════════════════ telemetry capture (compile-time rename) ═══════ */

static payload_v3_telemetry_input_t g_cap;
static char g_cap_str[40][160];
static int  g_cap_nstr;
static uint64_t g_cap_calls;

static const char *cap_dup(const char *s)
{
    if (s == NULL) return NULL;
    if (g_cap_nstr >= 40) abort();
    char *d = g_cap_str[g_cap_nstr++];
    snprintf(d, 160, "%s", s);
    return d;
}

/* device_commands.c is compiled with -Dpayload_v3_build_telemetry=h_capture_build_telemetry:
 * this records the input exactly as device_commands filled it, then calls the
 * REAL builder unchanged. */
bool h_capture_build_telemetry(char *out, size_t cap, const payload_v3_telemetry_input_t *in, char *err, size_t ecap);
bool h_capture_build_telemetry(char *out, size_t cap, const payload_v3_telemetry_input_t *in, char *err, size_t ecap)
{
    g_cap_calls++;
    g_cap_nstr = 0;
    g_cap = *in;
    g_cap.device = cap_dup(in->device);
    g_cap.last_disc_reason = cap_dup(in->last_disc_reason);
    g_cap.evq_sd_state = cap_dup(in->evq_sd_state);
    g_cap.evq_head_block = cap_dup(in->evq_head_block);
    g_cap.evq_blocked_reason = cap_dup(in->evq_blocked_reason);
    g_cap.evq_corrupt_medium = cap_dup(in->evq_corrupt_medium);
    g_cap.last_wd_reboot_reason = cap_dup(in->last_wd_reboot_reason);
    g_cap.clock_source = cap_dup(in->clock_source);
    g_cap.firmware = cap_dup(in->firmware);
    g_cap.script_sha256 = cap_dup(in->script_sha256);
    g_cap.script_version = cap_dup(in->script_version);
    g_cap.script_built_against_fw = cap_dup(in->script_built_against_fw);
    g_cap.script_installed_on_fw = cap_dup(in->script_installed_on_fw);
    for (size_t i = 0; i < in->attached_count && i < PAYLOAD_V3_MAX_ATTACHED; i++) {
        g_cap.attached[i].channel = cap_dup(in->attached[i].channel);
        g_cap.attached[i].sensor_id = cap_dup(in->attached[i].sensor_id);
        g_cap.attached[i].firmware = cap_dup(in->attached[i].firmware);
        g_cap.attached[i].name = cap_dup(in->attached[i].name);
    }
    return payload_v3_build_telemetry(out, cap, in, err, ecap);
}

/* ════════════════════════ composition root ══════════════════════════════ */

typedef struct {
    bool     card;              /* card A mounted at boot */
    bool     keeper;
    bool     sync_runner;
    uint32_t heartbeat_s;
    bool     power_port;
    size_t   flash_total;
    uint64_t sd_total;
    uint32_t cid;               /* card at boot (0 = CID_A) */
} boot_t;

static uint64_t g_direct_drain_calls, g_direct_service_calls;

static void direct_drain(void) { g_direct_drain_calls++; sync_runner_drain(); }
static void direct_service(void) { g_direct_service_calls++; (void)event_log_sd_service(); }

static void wait_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void shim_setup(void)
{
    h_log_open("esp.log");
    h_log_set_hook(log_hook);
    h_nvs_set_path("nvs.dat");
    h_shim_trace_task("sync_runner");
    h_shim_trace_task("sd_keeper");
    h_shim_start("test", TEST_PRIO);
}

static esp_err_t g_init_err;

static void boot(const boot_t *o)
{
    shim_setup();
    if (o->flash_total) h_media.flash_total = o->flash_total;
    if (o->sd_total) h_media.sd_total = o->sd_total;
    mkdir("evstore", 0777);
    mkdir("cards", 0777);
    if (o->card) {
        mkdir("sdcard", 0777);
        h_media.sd_mounted = true;
        h_media.sd_cid = o->cid ? o->cid : CID_A;
    }
    xTaskCreate(broker_task, "mqtt_task", 6144, NULL, BROKER_PRIO, &B.task);
    B.latency_us = 20000;

    (void)event_log_init_ids();
    g_init_err = event_log_init();
    event_log_set_reset_notifier(device_commands_on_persistence_reset);

    g_power_port = o->power_port;
    device_commands_config_t cfg = {
        .read_env = p_read_env,
        .read_power = o->power_port ? p_read_power : NULL,
        .next_id = event_log_get_next_id_fn(),
        .store_event = event_log_get_store_event_fn(),
        .claim_next_event = p_claim,
        .mark_event_synced = p_synced,
        .mark_event_pending = p_pending,
        .quarantine_event = p_quarantine,
        .db_stats = event_log_get_db_stats_fn(),
        .sd_health = p_sd_health,
        .read_script_identity = p_script,
        .publish = broker_publish,
        .message_is_connected = m_is_connected,
        .error_disconnect_count = m_err_disc,
        .connection_stats = m_conn_stats,
        .set_publish_ack_handler = m_set_ack,
        .set_disconnect_handler = m_set_disc,
        .publish_refusal_stats = m_refusal_stats,
        .topic_root = TOPIC,
        .device_id = "AMBYTE_240AC4E0517A",
        .protocol_id = "evq-integ",
        .device_name = "evq-integ",
        .device_version = "1",
        .device_firmware = "1",
        .timezone = "",
        .uart_text_query = p_uart_text,
        .last_wd_reboot_reason = p_wd_reason,
        .watchdog_armed = sync_runner_watchdog_armed,
    };
    CHECK(device_commands_init(&cfg) == ESP_OK, "device_commands_init");
    if (o->keeper) CHECK(event_log_sd_keeper_start() == ESP_OK, "keeper start");
    if (o->sync_runner) {
        CHECK(sync_runner_start(o->heartbeat_s) == ESP_OK, "sync_runner_start");
        sync_runner_boot_complete();
    }
}

static evlog_health_t health(void)
{
    evlog_health_t h;
    memset(&h, 0, sizeof h);
    CHECK(event_log_health(&h) == ESP_OK, "event_log_health");
    return h;
}

static void cursor(uint32_t *seq, uint32_t *off)
{
    uint32_t t = 0;
    (void)event_log_cursor_info(seq, off, &t);
}

static void card_remove(void)
{
    char dst[64];
    snprintf(dst, sizeof dst, "cards/%08x", (unsigned)h_media.sd_cid);
    h_media.sd_mounted = false;
    if (rename("sdcard", dst) != 0) { CHECK(false, "card_remove rename: %s", strerror(errno)); }
    h_trace("card", "removed", (int64_t)h_media.sd_cid);
    event_log_sd_notify();                  /* app_on_sd_state_change */
}

static void card_insert(uint32_t cid)
{
    char src[64];
    snprintf(src, sizeof src, "cards/%08x", (unsigned)cid);
    struct stat st;
    if (stat(src, &st) == 0) { if (rename(src, "sdcard") != 0) CHECK(false, "card_insert rename"); }
    else mkdir("sdcard", 0777);
    h_media.sd_cid = cid;
    h_media.sd_mounted = true;
    h_trace("card", "inserted", (int64_t)cid);
    event_log_sd_notify();
}

/* ════════════════════════ printing ══════════════════════════════════════ */

static void print_health_json(FILE *f, const evlog_health_t *h)
{
    fprintf(f, "{\"available\":%d,\"write_full\":%d,\"pending_exact\":%d,\"storage_blocked\":%d,"
               "\"pending\":%lld,\"deliverable_pending\":%lld,\"flash_pending\":%lld,\"sd_pending\":%lld,"
               "\"reimport_pending\":%lld,\"next_id\":%lld,\"last_acked_id\":%lld,\"skipped\":%lld,"
               "\"dropped\":%lld,\"refused_full\":%lld,\"refused_media\":%lld,\"refused_too_large\":%lld,"
               "\"refused_unavailable\":%lld,\"quarantined_poison\":%lld,\"quarantined_malformed\":%lld,"
               "\"skipped_unindexed_gap\":%lld,\"corrupt_detected\":%lld,\"rd_seq\":%u,\"tail_seq\":%u,",
            h->available, h->write_full, h->pending_exact, h->storage_blocked,
            (long long)h->pending, (long long)h->deliverable_pending, (long long)h->flash_pending,
            (long long)h->sd_pending, (long long)h->reimport_pending, (long long)h->next_id,
            (long long)h->last_acked_id, (long long)h->skipped, (long long)h->dropped,
            (long long)h->refused_full, (long long)h->refused_media, (long long)h->refused_too_large,
            (long long)h->refused_unavailable, (long long)h->quarantined_poison,
            (long long)h->quarantined_malformed, (long long)h->skipped_unindexed_gap,
            (long long)h->corrupt_detected, (unsigned)h->rd_seq, (unsigned)h->tail_seq);
    fprintf(f, "\"sd_state\":");      json_str(f, event_log_sd_state_name(h->sd_state));
    fprintf(f, ",\"head_block\":");   json_str(f, event_log_block_name(h->head_block));
    fprintf(f, ",\"blocked_reason\":"); json_str(f, event_log_blocked_reason_name(h->blocked_reason));
    fprintf(f, ",\"corrupt_medium\":"); json_str(f, event_log_medium_name(h->corrupt_medium));
    fprintf(f, ",\"spool_files\":%u,\"spool_errors\":%u,\"mirror_used\":%u,\"reclaimed_files\":%u,"
               "\"archived_files\":%u,\"reimported_files\":%u,\"pressure_notifies\":%u,\"sd_bursts\":%u,"
               "\"index_segments\":%u,\"index_cap\":%u,\"sd_retired_names\":%u,\"sd_bad_copies\":%u}",
            h->spool_files, h->spool_errors, h->mirror_used, h->reclaimed_files, h->archived_files,
            h->reimported_files, h->pressure_notifies, h->sd_bursts, h->index_segments, h->index_cap,
            h->sd_retired_names, h->sd_bad_copies);
}

static void ev_health(const char *prefix, const evlog_health_t *h)
{
    printf("%s_HEALTH=", prefix);
    print_health_json(stdout, h);
    printf("\n");
    fflush(stdout);
}

/* ════════════════════════ delivery oracle (C side) ══════════════════════ */

static bool env_payload(const pub_t *p, bool v3, const char **ps, size_t *pl)
{
    const char *env = p->env;
    const char *end;
    if (v3) {
        if (strncmp(env, "{\"sample\":[", 11) != 0) return false;
        end = NULL;
        for (const char *q = strstr(env, "],\"timestamp\":\""); q; q = strstr(q + 1, "],\"timestamp\":\"")) end = q;
        if (end == NULL) return false;
        *ps = env + 11; *pl = (size_t)(end - *ps);
        return true;
    }
    const char *m = strstr(env, "\"metadata\":");
    if (m == NULL) return false;
    const char *d = strstr(m, ",\"data\":");
    if (d == NULL) return false;
    end = NULL;
    for (const char *q = strstr(d, "}],\"timestamp\":\""); q; q = strstr(q + 1, "}],\"timestamp\":\"")) end = q;
    if (end == NULL) return false;
    *ps = d + 8; *pl = (size_t)(end - *ps);
    return true;
}

static bool env_timestamp(const pub_t *p, char out[24])
{
    const char *t = NULL;
    for (const char *q = strstr(p->env, "\"timestamp\":\""); q; q = strstr(q + 1, "\"timestamp\":\"")) t = q;
    if (t == NULL) return false;
    memcpy(out, t + 13, 20); out[20] = '\0';
    return true;
}

typedef struct {
    size_t accepted, delivered, missing, mismatched, ts_mismatch, duplicates, foreign, published;
    bool   first_order_ok;
    int64_t first_missing;
} e2e_t;

static int64_t *g_allowed_foreign;
static size_t   g_nallowed_foreign;

static e2e_t verify_e2e(const char *row)
{
    e2e_t r = { .first_order_ok = true, .first_missing = -1 };
    r.accepted = g_nacc;
    /* id → accepted index (ids ascend with store order) */
    size_t last_first = 0;
    bool have_last = false;
    size_t *first_pub = calloc(g_nacc ? g_nacc : 1, sizeof *first_pub);
    bool *seen = calloc(g_nacc ? g_nacc : 1, sizeof *seen);
    bool *deliv = calloc(g_nacc ? g_nacc : 1, sizeof *deliv);
    if (!first_pub || !seen || !deliv) abort();
    size_t event_pubs = 0;
    for (size_t i = 0; i < g_npub; i++) {
        pub_t *p = &g_pub[i];
        if (p->msg_id == 0) continue;               /* direct TELEMETRY */
        event_pubs++;
        /* binary search the accepted id */
        size_t lo = 0, hi = g_nacc;
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (g_acc[mid].id < p->mid) lo = mid + 1; else hi = mid; }
        if (lo >= g_nacc || g_acc[lo].id != p->mid) {
            bool allowed = false;
            for (size_t k = 0; k < g_nallowed_foreign; k++) if (g_allowed_foreign[k] == p->mid) allowed = true;
            if (!allowed) r.foreign++;
            continue;
        }
        rec_t *a = &g_acc[lo];
        const char *ps; size_t pl;
        if (!env_payload(p, a->v3, &ps, &pl) || pl != strlen(a->payload) || memcmp(ps, a->payload, pl) != 0) r.mismatched++;
        char ts[24], want[24];
        time_t sec = (time_t)(a->start_ms / 1000);
        struct tm tmv;
        gmtime_r(&sec, &tmv);
        strftime(want, sizeof want, "%Y-%m-%dT%H:%M:%SZ", &tmv);
        if (!env_timestamp(p, ts) || strcmp(ts, want) != 0) r.ts_mismatch++;
        if (!seen[lo]) { seen[lo] = true; first_pub[lo] = i; } else r.duplicates++;
        if (p->outcome == OUT_OK) deliv[lo] = true;
    }
    for (size_t k = 0; k < g_nacc; k++) {
        if (deliv[k]) r.delivered++;
        else { r.missing++; if (r.first_missing < 0) r.first_missing = g_acc[k].id; }
        if (seen[k]) {
            if (have_last && first_pub[k] < last_first) r.first_order_ok = false;
            last_first = first_pub[k];
            have_last = true;
        } else {
            r.first_order_ok = false;
        }
    }
    r.published = event_pubs;
    free(first_pub); free(seen); free(deliv);
    printf("%s_E2E={\"accepted\":%zu,\"delivered\":%zu,\"missing\":%zu,\"first_missing\":%lld,"
           "\"mismatched\":%zu,\"timestamp_mismatch\":%zu,\"duplicates\":%zu,\"foreign\":%zu,"
           "\"event_publishes\":%zu,\"first_delivery_order_ok\":%s}\n",
           row, r.accepted, r.delivered, r.missing, (long long)r.first_missing, r.mismatched, r.ts_mismatch,
           r.duplicates, r.foreign, r.published, r.first_order_ok ? "true" : "false");
    fflush(stdout);
    return r;
}

static void dump_manifests(void)
{
    FILE *a = fopen("accepted.jsonl", "w"), *ap = fopen("accepted_payloads.txt", "w");
    for (size_t i = 0; a && ap && i < g_nacc; i++) {
        fprintf(a, "{\"id\":%lld,\"start_ms\":%lld,\"end_ms\":%lld,\"v3\":%s,\"cmd_raw\":",
                (long long)g_acc[i].id, (long long)g_acc[i].start_ms, (long long)g_acc[i].end_ms,
                g_acc[i].v3 ? "true" : "false");
        json_str(a, g_acc[i].cmd);
        fprintf(a, ",\"metadata\":");
        if (g_acc[i].meta) json_str(a, g_acc[i].meta); else fprintf(a, "null");
        fprintf(a, "}\n");
        fprintf(ap, "%s\n", g_acc[i].payload);
    }
    if (a) fclose(a);
    if (ap) fclose(ap);
    FILE *p = fopen("published.jsonl", "w"), *e = fopen("envelopes.txt", "w");
    for (size_t i = 0; p && e && i < g_npub; i++) {
        fprintf(p, "{\"n\":%zu,\"vt_us\":%llu,\"msg_id\":%d,\"measure_id\":%lld,\"len\":%zu,\"outcome\":%d,"
                   "\"out_count\":%d,\"out_bytes\":%zu}\n",
                i, (unsigned long long)g_pub[i].vt_us, g_pub[i].msg_id, (long long)g_pub[i].mid,
                g_pub[i].len, g_pub[i].outcome, g_pub[i].out_count, g_pub[i].out_bytes);
        fprintf(e, "%s\n", g_pub[i].env);
    }
    if (p) fclose(p);
    if (e) fclose(e);
}

static void ev_common(void)
{
    evlog_health_t h = health();
    ev_health("FINAL", &h);
    EV("ACCEPTED", "%zu", g_nacc);
    EV("STORED_BYTES", "%llu", (unsigned long long)g_stored_bytes);
    EV("REFUSED", "{\"no_mem\":%lld,\"fail\":%lld,\"invalid_size\":%lld,\"other\":%lld}",
       (long long)g_refused[0], (long long)g_refused[1], (long long)g_refused[2], (long long)g_refused[3]);
    EV("CLAIMS", "{\"total\":%llu,\"ok\":%llu,\"not_found\":%llu,\"not_finished\":%llu,\"other\":%llu,\"gate_closed\":%llu}",
       (unsigned long long)g_claims, (unsigned long long)g_claims_ok, (unsigned long long)g_claims_nf,
       (unsigned long long)g_claims_nfin, (unsigned long long)g_claims_other, (unsigned long long)g_claims_gate_closed);
    EV("MARKS", "{\"synced\":%llu,\"pending\":%llu,\"quarantine\":%llu}",
       (unsigned long long)g_marks_synced, (unsigned long long)g_marks_pending, (unsigned long long)g_quarantines);
    EV("BROKER", "{\"publishes\":%zu,\"max_outstanding_slots\":%d,\"max_outstanding_bytes\":%zu,"
                 "\"window_violations\":%d,\"early_acks\":%u,\"refused_total\":%u,\"connects\":%u}",
       g_npub, B.max_out_count, B.max_out_bytes, B.window_violations, B.early_acks, B.refused_total, B.connects);
    EV("SD_BRACKET", "{\"io_begin\":%llu,\"io_end\":%llu,\"refused\":%llu,\"refs_now\":%d,\"refs_max\":%d,"
                     "\"sd_ops\":%llu,\"sd_ops_outside_bracket\":%llu,\"take_while_sd_ref\":%llu,\"unmount_with_refs\":%llu}",
       (unsigned long long)h_mstats.io_begin, (unsigned long long)h_mstats.io_end,
       (unsigned long long)h_mstats.io_begin_refused, h_mstats.refs_now, h_mstats.refs_max,
       (unsigned long long)h_mstats.sd_ops, (unsigned long long)h_mstats.sd_ops_outside_bracket,
       (unsigned long long)h_shim_stats.take_while_sd_ref, (unsigned long long)h_mstats.unmount_with_refs);
    EV("FLASH_MEDIA", "{\"enospc\":%llu,\"eio\":%llu,\"total\":%zu}",
       (unsigned long long)h_mstats.flash_enospc, (unsigned long long)h_mstats.flash_eio, h_media.flash_total);
    EV("SCHED", "{\"switches\":%llu,\"time_advances\":%llu,\"sem_contended\":%llu,\"sem_timeouts\":%llu,\"queue_full\":%llu}",
       (unsigned long long)h_shim_stats.switches, (unsigned long long)h_shim_stats.time_advances,
       (unsigned long long)h_shim_stats.sem_contended, (unsigned long long)h_shim_stats.sem_timeouts,
       (unsigned long long)h_shim_stats.queue_full);
    EV("VT_END_MS", "%llu", (unsigned long long)(h_vt_us() / 1000));
    EV("LOG_ERRORS", "%llu", (unsigned long long)g_log_errors);
    EV("DIRECT_CALLS", "{\"sync_runner_drain\":%llu,\"event_log_sd_service\":%llu}",
       (unsigned long long)g_direct_drain_calls, (unsigned long long)g_direct_service_calls);
    /* The sdcard_io_begin/end bracket must balance exactly (sd_card.h contract). */
    CHECK(h_mstats.refs_now == 0 && h_mstats.io_begin == h_mstats.io_end,
          "SD refcount unbalanced: begin=%llu end=%llu now=%d",
          (unsigned long long)h_mstats.io_begin, (unsigned long long)h_mstats.io_end, h_mstats.refs_now);
    CHECK(h_mstats.sd_ops_outside_bracket == 0, "SD operation outside an io_begin/io_end bracket (%llu)",
          (unsigned long long)h_mstats.sd_ops_outside_bracket);
    CHECK(h_shim_stats.take_while_sd_ref == 0, "mutex acquired while holding an SD ref (%llu)",
          (unsigned long long)h_shim_stats.take_while_sd_ref);
    dump_manifests();
}

/* Store until `bytes` of records were accepted, one record per `cadence_ms`. */
static void build_backlog(uint64_t bytes, uint32_t cadence_ms, uint64_t min_vt_ms)
{
    while (g_stored_bytes < bytes || h_vt_us() / 1000 < min_vt_ms) {
        esp_err_t e = produce();
        CHECK(e == ESP_OK, "store refused during backlog build: %s (accepted=%zu)", esp_err_to_name(e), g_nacc);
        if (e != ESP_OK) break;
        wait_ms(cadence_ms);
    }
}

static bool delivered_all(void)
{
    evlog_health_t h = health();
    size_t slots = 0, bytes = 0;
    device_commands_window_status(&slots, &bytes);
    return h.pending == 0 && slots == 0 && B.npend == 0;
}

static void drain_direct_until_empty(uint32_t max_iter)
{
    for (uint32_t i = 0; i < max_iter; i++) {
        direct_drain();
        wait_ms(50);
        if (delivered_all()) {
            direct_drain();             /* apply the last completions */
            if (delivered_all()) return;
        }
    }
    CHECK(false, "direct drain did not empty the queue in %u iterations", max_iter);
}

static bool wait_until_delivered(uint32_t timeout_s)
{
    for (uint32_t s = 0; s < timeout_s; s++) {
        if (delivered_all()) return true;
        wait_ms(1000);
    }
    return delivered_all();
}

/* ════════════════════════ scenarios ═════════════════════════════════════ */

/* I1 — SD_ONLY + flash backlog, direct drain → PUBACK callbacks → completion queue. */
static void sc_I1(void)
{
    boot(&(boot_t){ .card = true, .keeper = true });
    build_backlog(5ULL * h_media.flash_total / 2, 2000, 0);
    evlog_health_t h0 = health();
    ev_health("PRE_DRAIN", &h0);
    CHECK(h0.sd_pending > 0 && h0.flash_pending > 0, "need an SD_ONLY + flash backlog (sd=%lld flash=%lld)",
          (long long)h0.sd_pending, (long long)h0.flash_pending);
    CHECK(h0.reclaimed_files > 0, "pressure reclaim never ran");
    CHECK(g_refused[0] + g_refused[1] + g_refused[2] + g_refused[3] == 0, "stores refused with SD room");
    CHECK(h0.pending == (int64_t)g_nacc, "pending %lld != accepted %zu", (long long)h0.pending, g_nacc);
    B.early_every = 7;
    broker_connect();
    wait_ms(100);
    drain_direct_until_empty(200000);
    e2e_t r = verify_e2e("I1");
    CHECK(r.missing == 0 && r.mismatched == 0 && r.ts_mismatch == 0 && r.foreign == 0, "I1 delivery");
    CHECK(r.duplicates == 0 && r.first_order_ok && r.published == g_nacc, "I1 exact in-order publication");
    CHECK(B.window_violations == 0 && B.max_out_count <= (int)PUBLISH_WINDOW_SLOTS, "window bound");
    CHECK(B.early_acks > 0, "early-ACK path not exercised");
    ev_common();
}

/* I2 — PUBACK reason 0x87: refusal hold, records stay pending, cursor frozen. */
static void sc_I2(void)
{
    boot(&(boot_t){ .card = true, .keeper = true, .sync_runner = true });
    build_backlog(3ULL * h_media.flash_total / 2, 3000, 0);
    uint32_t s0, o0;
    cursor(&s0, &o0);
    evlog_health_t h0 = health();
    uint64_t refuse_window_us = 20ULL * 60 * 1000000;
    B.refuse_until_us = h_vt_us() + refuse_window_us;
    uint64_t t_connect = h_vt_us();
    broker_connect();
    bool moved_during_refusal = false, pending_changed = false, hold_seen = false;
    int64_t max_hold_ms = 0;
    uint64_t claims_during_hold = 0;
    uint64_t first_ok_vt = 0;
    uint32_t max_refusals = 0;
    for (int i = 0; i < 4 * 60 * 6; i++) {           /* 10-s samples, up to 4 h */
        uint64_t claims_before = g_claims;
        int64_t hold_before = 0;
        device_commands_refusal_status(NULL, &hold_before, NULL);
        wait_ms(10000);
        uint32_t refusals = 0; int64_t hold = 0, last_id = 0;
        device_commands_refusal_status(&refusals, &hold, &last_id);
        if (refusals > max_refusals) max_refusals = refusals;
        if (hold > 0) hold_seen = true;
        if (hold > max_hold_ms) max_hold_ms = hold;
        if (hold_before > 10000 && hold > 0) claims_during_hold += g_claims - claims_before;
        bool any_ok = false;
        for (size_t k = 0; k < g_npub; k++) if (g_pub[k].outcome == OUT_OK && g_pub[k].msg_id != 0) { any_ok = true; if (!first_ok_vt) first_ok_vt = g_pub[k].vt_us; break; }
        uint32_t s, o;
        cursor(&s, &o);
        if (!any_ok) {
            if (s != s0 || o != o0) moved_during_refusal = true;
            if (health().pending != h0.pending) pending_changed = true;
        }
        if (any_ok && delivered_all()) break;
    }
    size_t refused_pubs = 0;
    for (size_t k = 0; k < g_npub; k++) if (g_pub[k].outcome == OUT_REFUSED) refused_pubs++;
    EV("I2_REFUSAL", "{\"refused_publishes\":%zu,\"max_refusals_since_ok\":%u,\"max_hold_ms\":%lld,"
                     "\"claims_while_held\":%llu,\"cursor_moved_before_clean_ack\":%s,\"pending_changed_before_clean_ack\":%s,"
                     "\"first_clean_ack_after_connect_ms\":%llu,\"refuse_window_ms\":%llu}",
       refused_pubs, max_refusals, (long long)max_hold_ms, (unsigned long long)claims_during_hold,
       moved_during_refusal ? "true" : "false", pending_changed ? "true" : "false",
       (unsigned long long)(first_ok_vt ? (first_ok_vt - t_connect) / 1000 : 0),
       (unsigned long long)(refuse_window_us / 1000));
    CHECK(refused_pubs > 0 && hold_seen && max_refusals > 0, "refusal hold never engaged");
    CHECK(!moved_during_refusal, "cursor moved before a clean re-ACK");
    CHECK(!pending_changed, "pending changed before a clean re-ACK");
    CHECK(claims_during_hold == 0, "claims while the refusal hold was active");
    CHECK(first_ok_vt >= B.refuse_until_us, "an ACK was accepted inside the refusal window");
    CHECK(delivered_all(), "I2 did not finish");
    e2e_t r = verify_e2e("I2");
    CHECK(r.missing == 0 && r.mismatched == 0 && r.ts_mismatch == 0 && r.foreign == 0 && r.first_order_ok, "I2 delivery");
    /* every refused id was republished and then cleanly ACKed */
    size_t refused_never_ok = 0;
    for (size_t k = 0; k < g_npub; k++) {
        if (g_pub[k].outcome != OUT_REFUSED) continue;
        bool ok = false;
        for (size_t j = k + 1; j < g_npub; j++) if (g_pub[j].mid == g_pub[k].mid && g_pub[j].outcome == OUT_OK) { ok = true; break; }
        if (!ok) refused_never_ok++;
    }
    EV("I2_REFUSED_NEVER_REACKED", "%zu", refused_never_ok);
    CHECK(refused_never_ok == 0, "a refused id never got a clean re-ACK");
    ev_common();
}

/* I3 — disconnect with a full window: abort + revert; redelivery covers every unacked record. */
static void sc_I3(void)
{
    boot(&(boot_t){ .card = true, .keeper = true, .sync_runner = true });
    build_backlog(3ULL * h_media.flash_total / 2, 3000, 0);
    uint32_t s0, o0;
    cursor(&s0, &o0);
    evlog_health_t h0 = health();
    B.hold_acks = true;                       /* broker withholds PUBACKs: the window fills */
    broker_connect();
    wait_ms(15000);
    int cnt; size_t bytes;
    outstanding(&cnt, &bytes);
    size_t dc_slots = 0, dc_bytes = 0;
    device_commands_window_status(&dc_slots, &dc_bytes);
    int64_t unacked[64]; int nun = 0;
    for (int i = 0; i < B.npend; i++) unacked[nun++] = g_pub[B.pend[i].pub].mid;
    bool full = cnt == (int)PUBLISH_WINDOW_SLOTS || bytes + 2048 > PUBLISH_WINDOW_BYTES;
    EV("I3_WINDOW_AT_DISCONNECT", "{\"broker_outstanding\":%d,\"broker_bytes\":%zu,\"dc_slots\":%zu,\"dc_bytes\":%zu,\"full\":%s}",
       cnt, bytes, dc_slots, dc_bytes, full ? "true" : "false");
    CHECK(full, "window not full at disconnect");
    CHECK((int)dc_slots == cnt, "device_commands and broker disagree on outstanding slots");
    size_t npub_at_disc = g_npub;
    broker_disconnect();
    B.hold_acks = false;
    wait_ms(2000);
    device_commands_window_status(&dc_slots, &dc_bytes);
    uint32_t s1, o1;
    cursor(&s1, &o1);
    evlog_health_t h1 = health();
    EV("I3_AFTER_DISCONNECT", "{\"dc_slots\":%zu,\"cursor_unchanged\":%s,\"pending_unchanged\":%s,\"marks_pending\":%llu}",
       dc_slots, (s1 == s0 && o1 == o0) ? "true" : "false", h1.pending == h0.pending ? "true" : "false",
       (unsigned long long)g_marks_pending);
    CHECK(dc_slots == 0, "window not reverted after disconnect");
    CHECK(s1 == s0 && o1 == o0 && h1.pending == h0.pending, "cursor/pending moved without ACKs");
    CHECK(g_marks_pending >= (uint64_t)nun, "not every unacked slot was reverted");
    broker_connect();
    CHECK(wait_until_delivered(4 * 3600), "I3 did not finish");
    /* The first publications after reconnect are exactly the reverted ids, FIFO. */
    bool prefix_ok = g_npub >= npub_at_disc + (size_t)nun;
    for (int i = 0; prefix_ok && i < nun; i++) if (g_pub[npub_at_disc + (size_t)i].mid != unacked[i]) prefix_ok = false;
    size_t covered = 0;
    for (int i = 0; i < nun; i++) {
        for (size_t k = npub_at_disc; k < g_npub; k++) if (g_pub[k].mid == unacked[i] && g_pub[k].outcome == OUT_OK) { covered++; break; }
    }
    EV("I3_REDELIVERY", "{\"unacked_at_disconnect\":%d,\"redelivered_ok\":%zu,\"reverted_first_fifo\":%s}",
       nun, covered, prefix_ok ? "true" : "false");
    CHECK(covered == (size_t)nun, "redelivered set does not cover every unacked record");
    CHECK(prefix_ok, "reverted slots were not re-claimed first, in order");
    e2e_t r = verify_e2e("I3");
    CHECK(r.missing == 0 && r.mismatched == 0 && r.ts_mismatch == 0 && r.foreign == 0 && r.first_order_ok, "I3 delivery");
    ev_common();
}

/* I4 — the head waits on an unreadable SD copy: NOT_FINISHED, no spin, no watchdog. */
static void sc_I4(void)
{
    boot(&(boot_t){ .card = true, .keeper = true, .sync_runner = true });
    /* Backlog in < 1 h: with deliverable records and no PUBACK for an hour the
     * real no-PUBACK watchdog (correctly) reboots, which this harness treats as
     * a failure. The long `since` accrues below while the head WAITS. */
    build_backlog(8ULL * h_media.flash_total / 5, 2000, 0);
    evlog_health_t h0 = health();
    ev_health("I4_BACKLOG", &h0);
    CHECK(h0.sd_pending > 0, "no SD_ONLY head");
    uint64_t ep_sr0 = g_log_sr_wait, ep_el0 = g_log_el_wait, ep_claims0 = g_claims;
    size_t ep_tr0 = h_trace_count();
    card_remove();
    broker_connect();
    wait_ms(1000);
    /* Let the production watchdog task evaluate once a minute past its 1-h
     * no-PUBACK timeout while the head waits (it must not reboot). */
    while (h_vt_us() < 62ULL * 60 * 1000000) {
        (void)produce();
        wait_ms(5 * 60000);
    }
    /* 10 simulated minutes with the drain task running; one store per minute. */
    size_t tr0 = h_trace_count();
    uint64_t claims0 = g_claims, nfin0 = g_claims_nfin, nf0 = g_claims_nf, ok0 = g_claims_ok;
    uint64_t sr0 = g_log_sr_wait, el0 = g_log_el_wait;
    size_t pub0 = g_npub;
    uint64_t t0 = h_vt_us();
    int stores_ok = 0;
    for (int m = 0; m < 10; m++) {
        if (produce() == ESP_OK) stores_ok++;
        wait_ms(60000);
    }
    uint64_t t1 = h_vt_us();
    uint64_t wakes_notify = 0, wakes_timeout = 0, drains = 0;
    for (size_t i = tr0; i < h_trace_count(); i++) {
        const h_trace_t *e = h_trace_at(i);
        if (strcmp(e->task, "sync_runner") != 0) continue;
        if (strcmp(e->kind, "wake") == 0) { if (strcmp(e->a, "notify") == 0) wakes_notify++; else wakes_timeout++; }
        if (strcmp(e->kind, "drain_enter") == 0) drains++;
    }
    uint64_t claims = g_claims - claims0;
    uint64_t w_nfin = g_claims_nfin - nfin0, w_nf = g_claims_nf - nf0, w_ok = g_claims_ok - ok0;
    uint64_t w_sr = g_log_sr_wait - sr0, w_el = g_log_el_wait - el0;
    uint64_t ep_wakes = 0;
    for (size_t i = ep_tr0; i < h_trace_count(); i++) {
        const h_trace_t *e = h_trace_at(i);
        if (strcmp(e->task, "sync_runner") == 0 && strcmp(e->kind, "wake") == 0) ep_wakes++;
    }
    uint64_t ep_claims = g_claims - ep_claims0, ep_sr = g_log_sr_wait - ep_sr0, ep_el = g_log_el_wait - ep_el0;
    evlog_health_t h1 = health();
    cmd_result_t direct = cmd_mqtt_publish_next_event();
    bool a = false, c = false;
    int64_t wp = 0, since = 0;
    bool reboot = sync_runner_wd_should_reboot(SYNC_WD_TIMEOUT_MS, &a, &c, &wp, &since);
    EV("I4_WINDOW", "{\"simulated_ms\":%llu,\"claims\":%llu,\"claims_not_finished\":%llu,\"claims_not_found\":%llu,"
                    "\"claims_ok\":%llu,\"drain_wakes_notify\":%llu,\"drain_wakes_fallback\":%llu,\"drain_passes\":%llu,"
                    "\"sync_runner_wait_logs\":%llu,\"event_log_wait_logs\":%llu,\"publishes\":%zu,\"stores_ok\":%d}",
       (unsigned long long)((t1 - t0) / 1000), (unsigned long long)claims,
       (unsigned long long)w_nfin, (unsigned long long)w_nf,
       (unsigned long long)w_ok, (unsigned long long)wakes_notify, (unsigned long long)wakes_timeout,
       (unsigned long long)drains, (unsigned long long)w_sr, (unsigned long long)w_el,
       g_npub - pub0, stores_ok);
    EV("I4_EPISODE", "{\"simulated_ms\":%llu,\"drain_wakes\":%llu,\"claims\":%llu,\"sync_runner_wait_logs\":%llu,"
                     "\"event_log_wait_logs\":%llu,\"state_changes\":1}",
       (unsigned long long)((t1 - h_trace_at(ep_tr0)->vt_us) / 1000), (unsigned long long)ep_wakes,
       (unsigned long long)ep_claims, (unsigned long long)ep_sr, (unsigned long long)ep_el);
    ev_health("I4_WAITING", &h1);
    printf("I4_DIRECT_PUBLISH={\"status\":\"%s\",\"message\":", esp_err_to_name(direct.status));
    json_str(stdout, direct.message);
    printf("}\n");
    EV("I4_WATCHDOG", "{\"should_reboot\":%s,\"power_ok\":%s,\"clock_ok\":%s,\"deliverable_pending\":%lld,"
                      "\"since_ms\":%lld,\"timeout_ms\":%lld}",
       reboot ? "true" : "false", a ? "true" : "false", c ? "true" : "false", (long long)wp,
       (long long)since, (long long)SYNC_WD_TIMEOUT_MS);
    CHECK(direct.status == ESP_ERR_NOT_FINISHED, "cmd_mqtt_publish_next_event returned %s", esp_err_to_name(direct.status));
    CHECK(strstr(direct.message, "no pending") == NULL, "NOT_FINISHED mapped to 'no pending measurements'");
    CHECK(w_nf == 0 && w_ok == 0, "claim returned NOT_FOUND/OK while the head waits");
    CHECK(w_nfin == claims && claims > 0, "every claim must be NOT_FINISHED");
    CHECK(claims <= wakes_notify + wakes_timeout + 1, "busy-spin: %llu claims for %llu wakes",
          (unsigned long long)claims, (unsigned long long)(wakes_notify + wakes_timeout));
    CHECK(ep_claims <= ep_wakes + 1, "busy-spin over the whole wait episode");
    CHECK(w_sr == 0 && w_el == 0, "log lines inside the window without a state change");
    CHECK(ep_sr == 1 && ep_el == 1, "the wait episode (one state change) must log exactly once per layer");
    CHECK(h1.pending > 0 && h1.deliverable_pending == 0, "pending must stay > 0, deliverable 0");
    CHECK(h1.sd_pending > 0 && strcmp(event_log_block_name(h1.head_block), "sd_absent") == 0, "head_block");
    CHECK(g_npub == pub0, "published while the head waits");
    CHECK(!reboot && a && c && since > SYNC_WD_TIMEOUT_MS, "watchdog predicate");
    CHECK(stores_ok == 10, "stores must keep succeeding into flash while the head waits");
    /* Same card back: delivery resumes at the same cursor, original order. */
    card_insert(CID_A);
    CHECK(wait_until_delivered(6 * 3600), "I4 did not finish after reinsertion");
    e2e_t r = verify_e2e("I4");
    CHECK(r.missing == 0 && r.mismatched == 0 && r.ts_mismatch == 0 && r.foreign == 0 && r.first_order_ok && r.duplicates == 0,
          "I4 delivery after reinsertion");
    EV("I4_LOGS_TOTAL", "{\"sync_runner_wait_logs\":%llu,\"event_log_wait_logs\":%llu}",
       (unsigned long long)g_log_sr_wait, (unsigned long long)g_log_el_wait);
    ev_common();
}

/* ── I5 / I9 snapshots ── */

static void snapshot(const char *name)
{
    evlog_health_t h1 = health();
    size_t before = g_npub;
    uint64_t cap_before = g_cap_calls;
    /* The direct TELEMETRY path needs a live transport; no drain runs here, so
     * raising the link for this one publish cannot cause a claim. */
    bool was_connected = B.connected;
    B.connected = true;
    cmd_result_t r = cmd_publish_status_event();
    B.connected = was_connected;
    evlog_health_t h2 = health();
    /* emit_status_event allocates the heartbeat's own measure_id first. */
    CHECK(h2.next_id == h1.next_id + 1, "heartbeat id allocation");
    h2.next_id = h1.next_id;
    const char *env = NULL;
    for (size_t i = before; i < g_npub; i++) if (g_pub[i].msg_id == 0) env = g_pub[i].env;
    static char cli[1024];
    int cl = evq_render_health_text(&h1, cli, sizeof cli);
    printf("SNAPSHOT_JSON={\"name\":\"%s\",\"emit_status\":\"%s\",\"stable\":%s,\"captured\":%s,\"health\":",
           name, esp_err_to_name(r.status), memcmp(&h1, &h2, sizeof h1) == 0 ? "true" : "false",
           g_cap_calls > cap_before ? "true" : "false");
    print_health_json(stdout, &h1);
    printf(",\"capture\":{\"evq_valid\":%s,\"pending\":%lld,\"pending_exact\":%s,\"storage_blocked\":%s,"
           "\"deliverable_pending\":%lld,\"flash_pending\":%lld,\"sd_pending\":%lld,\"reimport_pending\":%lld,"
           "\"refused_full\":%lld,\"refused_media\":%lld,\"refused_too_large\":%lld,\"refused_unavailable\":%lld,"
           "\"quarantined_poison\":%lld,\"quarantined_malformed\":%lld,\"skipped_unindexed_gap\":%lld,"
           "\"corrupt_detected\":%lld,\"spool_files\":%u,\"spool_errors\":%u,\"mirror_used\":%u,"
           "\"reclaimed_files\":%u,\"archived_files\":%u,\"reimported_files\":%u,"
           "\"sd_retired_names\":%u,\"sd_bad_copies\":%u,\"sd_state\":",
           g_cap.evq_valid ? "true" : "false", (long long)g_cap.evq_pending, g_cap.evq_pending_exact ? "true" : "false",
           g_cap.evq_storage_blocked ? "true" : "false", (long long)g_cap.evq_deliverable_pending,
           (long long)g_cap.evq_flash_pending, (long long)g_cap.evq_sd_pending, (long long)g_cap.evq_reimport_pending,
           (long long)g_cap.evq_refused_full, (long long)g_cap.evq_refused_media, (long long)g_cap.evq_refused_too_large,
           (long long)g_cap.evq_refused_unavailable, (long long)g_cap.evq_quarantined_poison,
           (long long)g_cap.evq_quarantined_malformed, (long long)g_cap.evq_skipped_unindexed_gap,
           (long long)g_cap.evq_corrupt_detected, g_cap.evq_spool_files, g_cap.evq_spool_errors, g_cap.evq_mirror_used,
           g_cap.evq_reclaimed_files, g_cap.evq_archived_files, g_cap.evq_reimported_files,
           g_cap.evq_sd_retired_names, g_cap.evq_sd_bad_copies);
    json_str(stdout, g_cap.evq_sd_state);
    printf(",\"head_block\":"); json_str(stdout, g_cap.evq_head_block);
    printf(",\"blocked_reason\":"); json_str(stdout, g_cap.evq_blocked_reason);
    printf(",\"corrupt_medium\":"); json_str(stdout, g_cap.evq_corrupt_medium);
    printf("},\"cli_len\":%d,\"cli\":", cl);
    json_str(stdout, cl >= 0 ? cli : "");
    printf(",\"envelope\":%s}\n", env ? env : "null");
    fflush(stdout);
    CHECK(r.status == ESP_OK && env != NULL, "status emit failed for %s: %s", name, r.message);
    CHECK(memcmp(&h1, &h2, sizeof h1) == 0, "health changed across the snapshot %s", name);
    CHECK(cl > 0, "CLI render refused for %s", name);
}

/* Fill flash until stores are refused AND the blocked state is latched
 * (storage_blocked), then return the refusals seen (at least `min_refusals`). */
static int fill_until_refused(uint32_t cadence_ms, int min_refusals)
{
    int refused = 0;
    for (int i = 0; i < 20000; i++) {
        if (produce() != ESP_OK) refused++;
        wait_ms(cadence_ms);
        if (refused >= min_refusals && health().storage_blocked) break;
    }
    return refused >= min_refusals && health().storage_blocked ? min_refusals : -1;
}

/* Rewrite one byte of every copy of the SD primary/mirror of the head segment. */
static int corrupt_files_in(const char *dir)
{
    int n = 0;
    DIR *d = opendir(dir);
    if (d == NULL) return 0;
    struct dirent *e;
    char names[256][64];
    int nn = 0;
    while ((e = readdir(d)) != NULL && nn < 256) {
        if (strstr(e->d_name, ".log") == NULL) continue;
        snprintf(names[nn++], 64, "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < nn; i++) {
        char p[160];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        FILE *f = fopen(p, "r+b");
        if (f == NULL) continue;
        /* Mid-file: inside a record's payload blob (alnum), so framing survives. */
        long mid = 0;
        if (fseek(f, 0, SEEK_END) == 0) mid = ftell(f) / 2;
        if (mid > 0 && fseek(f, mid, SEEK_SET) == 0) {
            int c = fgetc(f);
            if (c != EOF && c != '\n' && c != '\t' && fseek(f, mid, SEEK_SET) == 0) { fputc(c == '#' ? '%' : '#', f); n++; }
        }
        fclose(f);
    }
    return n;
}

static void st_normal(void)
{
    boot(&(boot_t){ .card = true, .keeper = true });
    for (int i = 0; i < 40; i++) { CHECK(produce() == ESP_OK, "store"); wait_ms(1000); }
    snapshot("normal");
    /* Stored heartbeat path: cmd_store_status_event → claim → publish. */
    evlog_health_t hb = health();
    cmd_result_t r = cmd_store_status_event();
    CHECK(r.status == ESP_OK, "cmd_store_status_event: %s", r.message);
    broker_connect();
    wait_ms(100);
    drain_direct_until_empty(10000);
    const char *env = NULL;
    for (size_t i = 0; i < g_npub; i++) if (g_pub[i].msg_id != 0 && strstr(g_pub[i].env, "ambyte.telemetry/1")) env = g_pub[i].env;
    printf("SNAPSHOT_JSON={\"name\":\"heartbeat_stored\",\"emit_status\":\"%s\",\"stable\":true,\"captured\":true,\"health\":",
           esp_err_to_name(r.status));
    print_health_json(stdout, &hb);
    printf(",\"capture\":null,\"cli_len\":-2,\"cli\":\"\",\"envelope\":%s}\n", env ? env : "null");
    CHECK(env != NULL, "stored heartbeat was not published");
    ev_common();
}

static void st_blocked_sd_unavailable(void)
{
    boot(&(boot_t){ .card = false, .keeper = true });
    CHECK(fill_until_refused(500, 3) == 3, "flash never filled");
    snapshot("blocked_sd_unavailable");
    ev_common();
}

static void st_blocked_sd_full(void)
{
    boot(&(boot_t){ .card = true, .keeper = true, .sd_total = 1536 * 1024 });   /* below reserve+need */
    CHECK(fill_until_refused(500, 3) == 3, "flash never filled");
    snapshot("blocked_sd_full");
    ev_common();
}

static void st_blocked_sd_error(void)
{
    boot(&(boot_t){ .card = true, .keeper = false });
    /* Media manipulation: the mirror directory is a regular file → mirror copy fails. */
    FILE *f = fopen("sdcard/evq", "w");
    if (f) { fputs("not a directory\n", f); fclose(f); }
    CHECK(event_log_sd_keeper_start() == ESP_OK, "keeper");
    CHECK(fill_until_refused(500, 3) == 3, "flash never filled");
    snapshot("blocked_sd_error");
    ev_common();
}

static void outage_to_sd_only(void)
{
    build_backlog(3ULL * h_media.flash_total / 2, 2000, 0);
    CHECK(health().sd_pending > 0, "no SD_ONLY segments");
}

/* The head waits on an SD_ONLY segment whose card was swapped for another. */
static void st_sd_mismatch_head(void)
{
    boot(&(boot_t){ .card = true, .keeper = true });
    outage_to_sd_only();
    card_remove();
    wait_ms(3000);
    card_insert(CID_B);                       /* a different card, empty, with room */
    wait_ms(1000);
    snapshot("sd_mismatch");
    ev_common();
}

/* flash_full_sd_mismatch: an obligation that only card A can satisfy, while
 * the delivery head itself is readable from flash and card B is mounted. A
 * rollback firmware moving the legacy cursor produces exactly that (the
 * passed SD_ONLY files become REIMPORT entries on card A). Built from media +
 * NVS only: boot 1 spools to card A and shuts down cleanly; the "rollback"
 * rewrites the legacy rd_seq/rd_off keys to the tail; card B replaces card A;
 * boot 2 fills flash with MQTT down. */
static void st_blocked_sd_mismatch(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        boot(&(boot_t){ .card = true, .keeper = true });
        outage_to_sd_only();
        uint32_t seq = 0, off = 0, tail = 0;
        (void)event_log_cursor_info(&seq, &off, &tail);
        CHECK(event_log_prepare_shutdown() == ESP_OK, "shutdown flush");
        FILE *f = fopen("tail.txt", "w");
        if (f) { fprintf(f, "%u\n", (unsigned)tail); fclose(f); }
        fflush(stdout);
        _exit(g_failures == 0 ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "boot-1 child failed");
    unsigned tail = 0;
    FILE *f = fopen("tail.txt", "r");
    if (f) { if (fscanf(f, "%u", &tail) != 1) tail = 0; fclose(f); }
    CHECK(tail > 1, "no tail from boot 1");
    h_nvs_set_path("nvs.dat");
    nvs_handle_t h;
    CHECK(nvs_open("evlog", NVS_READWRITE, &h) == ESP_OK, "nvs");
    nvs_set_u32(h, "rd_seq", tail);          /* what a rolled-back firmware writes */
    nvs_set_u32(h, "rd_off", 0);
    nvs_commit(h);
    nvs_close(h);
    char dst[64];
    snprintf(dst, sizeof dst, "cards/%08x", (unsigned)CID_A);
    mkdir("cards", 0777);
    CHECK(rename("sdcard", dst) == 0, "swap out card A");
    boot(&(boot_t){ .card = true, .keeper = true, .cid = CID_B });
    wait_ms(1000);
    snapshot("sd_mismatch_reimport");
    CHECK(fill_until_refused(500, 3) == 3, "flash never filled");
    snapshot("blocked_sd_mismatch");
    ev_common();
}

static void st_blocked_backlog_waiting(void)
{
    boot(&(boot_t){ .card = true, .keeper = true });
    outage_to_sd_only();
    card_remove();
    broker_connect();
    wait_ms(100);
    direct_drain();                           /* the claim meets the unreadable head */
    CHECK(fill_until_refused(500, 3) == 3, "flash never filled");
    direct_drain();
    snapshot("blocked_backlog_waiting");
    ev_common();
}

static void st_blocked_index_cap(void)
{
    boot(&(boot_t){ .card = true, .keeper = true });
    CHECK(fill_until_refused(500, 3) == 3, "flash never filled");
    snapshot("blocked_index_cap");
    ev_common();
}

static void st_blocked_transfer_pending(void)
{
    boot(&(boot_t){ .card = true, .keeper = false });   /* keeper has not run yet */
    CHECK(fill_until_refused(500, 3) == 3, "flash never filled");
    snapshot("blocked_transfer_pending");
    ev_common();
}

static void st_sd_lost_parked(void)
{
    boot(&(boot_t){ .card = true, .keeper = true });
    for (int i = 0; i < 20; i++) { produce(); wait_ms(1000); }
    h_media.sd_io_lost = true;                /* sd_card loss latch */
    wait_ms(1000);
    snapshot("sd_lost");
    h_media.sd_io_lost = false;
    event_log_set_sd_parked(true);            /* low-battery guard park + unmount */
    CHECK(sdcard_unmount() == ESP_OK, "unmount");
    wait_ms(1000);
    snapshot("sd_parked");
    CHECK(sdcard_mount() == ESP_OK, "mount");
    event_log_set_sd_parked(false);
    wait_ms(1000);
    ev_common();
}

static void st_backlog_corrupt(void)
{
    boot(&(boot_t){ .card = true, .keeper = true });
    outage_to_sd_only();
    int n = corrupt_files_in("sdcard/events") + corrupt_files_in("sdcard/evq");
    EV("CORRUPTED_SD_FILES", "%d", n);
    event_log_sd_notify();
    broker_connect();
    wait_ms(100);
    /* Re-verification happens per mount epoch: a remove/insert the keeper
     * observes (the real monitor polls every 2 s, so it always sees the gap). */
    card_remove();
    wait_ms(3000);
    card_insert(CID_A);
    wait_ms(1000);
    direct_drain();
    snapshot("sd_backlog_corrupt");
    ev_common();
}

static void st_corrupt_flash(void)
{
    boot(&(boot_t){ .card = false, .keeper = true });
    for (int i = 0; i < 60; i++) { produce(); wait_ms(1000); }   /* > 1 rotation, flash only */
    FILE *f = fopen("evstore/events/ev-000001.log", "r+b");
    if (f) { fseek(f, 10, SEEK_SET); fputc('#', f); fclose(f); }
    broker_connect();
    wait_ms(100);
    direct_drain();
    snapshot("corrupt_flash");
    ev_common();
}

static void st_refused(void)
{
    boot(&(boot_t){ .card = true, .keeper = true });
    for (int i = 0; i < 5; i++) { produce(); wait_ms(1000); }
    /* too large */
    size_t big = 70000;
    char *p = malloc(big + 1);
    memset(p, 'a', big); p[0] = '{'; p[1] = '"'; p[2] = 'x'; p[3] = '"'; p[4] = ':'; p[5] = '"'; p[big - 2] = '"'; p[big - 1] = '}'; p[big] = '\0';
    int64_t id = 0;
    (void)cmd_next_measure_id(&id);
    measurement_event_desc_t d = { .measure_id = id, .tag = MEASUREMENT_TAG_MEASUREMENT, .payload_json = p };
    cmd_result_t r = cmd_store_event(&d);
    CHECK(r.status == ESP_ERR_INVALID_SIZE, "oversize store: %s", esp_err_to_name(r.status));
    free(p);
    /* media error: one EIO on the next flash fsync */
    h_media.flash_eio_next_fsync = true;
    esp_err_t e = produce();
    CHECK(e == ESP_FAIL, "EIO store: %s", esp_err_to_name(e));
    snapshot("refused_media_too_large");
    ev_common();
}

static void st_unavailable(void)
{
    mkdir("evstore", 0777);
    FILE *f = fopen("evstore/events", "w");   /* the events dir is a file → store cannot open */
    if (f) { fputs("x", f); fclose(f); }
    boot(&(boot_t){ .card = true, .keeper = false });
    esp_err_t e = produce();
    CHECK(e == ESP_ERR_NOT_SUPPORTED, "store on an unavailable log: %s", esp_err_to_name(e));
    snapshot("unavailable");
    ev_common();
}

static void st_quarantined(void)
{
    /* Legacy SD import file with one malformed line (pre-upgrade residue). */
    mkdir("sdcard", 0777);
    mkdir("sdcard/events", 0777);
    FILE *f = fopen("sdcard/events/ev-000009.log", "w");
    if (f) {
        fprintf(f, "900001\tuart_1\tambit\tMEASUREMENT\tarrun 1\t1790380000000\t1790380001000\t\t{\"synthetic_test\":true,\"legacy\":1}\n");
        fprintf(f, "this line is not a v2 record\n");
        fclose(f);
    }
    boot(&(boot_t){ .card = true, .keeper = true });
    for (int i = 0; i < 5; i++) { produce(); wait_ms(1000); }
    wait_ms(61000);                           /* keeper period: legacy import */
    /* Poison: heap too fragmented for the head record → 30 strikes → quarantine. */
    broker_connect();
    wait_ms(100);
    h_media.heap_internal_largest = 1024;
    for (int i = 0; i < 40; i++) { direct_drain(); wait_ms(1000); }
    h_media.heap_internal_largest = 1024 * 1024;
    snapshot("quarantined");
    ev_common();
}

static pid_t reboot_child(void)
{
    fflush(stdout);
    pid_t pid = fork();
    return pid;
}

static void st_upgrade(void)
{
    /* Phase 1 (child "old boot"): store across several rotations, MQTT down. */
    pid_t pid = reboot_child();
    if (pid == 0) {
        boot(&(boot_t){ .card = false, .keeper = false });
        for (int i = 0; i < 80; i++) { produce(); wait_ms(1000); }
        fflush(stdout);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "phase-1 child failed");
    /* Lost index + a pre-upgrade gap (a middle file the old firmware deleted). */
    remove("evstore/evq.idx");
    remove("evstore/events/ev-000002.log");
    boot(&(boot_t){ .card = false, .keeper = false });
    snapshot("pending_inexact");
    broker_connect();
    wait_ms(100);
    drain_direct_until_empty(20000);
    CHECK(event_log_sd_keeper_start() == ESP_OK, "keeper");
    wait_ms(2000);
    snapshot("unindexed_gap");
    ev_common();
}

/* ── worst cases (I5 + I9) ── */

static uint8_t longest_value(const char *(*fn)(uint8_t))
{
    uint8_t bv = 0;
    for (int v = 0; v < 256; v++) if (strlen(fn((uint8_t)v)) > strlen(fn(bv))) bv = (uint8_t)v;
    return bv;
}

static void worst_health(evlog_health_t *w, int64_t v64)
{
    memset(w, 0, sizeof *w);
    w->available = w->write_full = w->storage_blocked = true;
    w->pending_exact = false;
    w->pending = w->deliverable_pending = w->flash_pending = w->sd_pending = w->reimport_pending = v64;
    w->next_id = w->last_acked_id = w->skipped = w->dropped = v64;
    w->refused_full = w->refused_media = w->refused_too_large = w->refused_unavailable = v64;
    w->quarantined_poison = w->quarantined_malformed = w->skipped_unindexed_gap = w->corrupt_detected = v64;
    w->rd_seq = w->tail_seq = UINT32_MAX;
    w->sd_state = longest_value(event_log_sd_state_name);
    w->head_block = longest_value(event_log_block_name);
    w->blocked_reason = longest_value(event_log_blocked_reason_name);
    w->corrupt_medium = longest_value(event_log_medium_name);
    w->spool_files = w->spool_errors = w->mirror_used = w->reclaimed_files = w->archived_files = UINT32_MAX;
    w->reimported_files = w->pressure_notifies = w->sd_bursts = w->index_segments = w->index_cap = UINT32_MAX;
    w->sd_retired_names = w->sd_bad_copies = UINT32_MAX;
}

static void fill_str(char *dst, size_t n, char c) { memset(dst, c, n); dst[n] = '\0'; }

static void sc_worst(void)
{
    boot(&(boot_t){ .card = true, .keeper = false });
    for (int i = 0; i < 3; i++) produce();
    snapshot("pre_worst");                    /* captures a real device_commands input */

    /* I9: CLI renderer at the production buffer (CLI.c cli_cmd_evlog: static
     * char text[1536]) must FIT the worst case; a deliberately small buffer must
     * refuse (-1) rather than truncate. */
    const int64_t vals[2] = { INT64_MAX, INT64_MIN };
    const char *vn[2] = { "INT64_MAX", "INT64_MIN" };
    for (int k = 0; k < 2; k++) {
        evlog_health_t w;
        worst_health(&w, vals[k]);
        static char prod[1536], big[8192], tiny[256];
        int rp = evq_render_health_text(&w, prod, sizeof prod);
        int rb = evq_render_health_text(&w, big, sizeof big);
        memset(tiny, 'Z', sizeof tiny);
        int rt = evq_render_health_text(&w, tiny, sizeof tiny);
        size_t tlen = strnlen(tiny, sizeof tiny);
        printf("WORST_CLI_JSON={\"counters\":\"%s\",\"cap\":%zu,\"rc_at_cap\":%d,\"true_len\":%d,"
               "\"small_cap\":%zu,\"rc_small\":%d,\"small_strlen\":%zu,\"text\":",
               vn[k], sizeof prod, rp, rb, sizeof tiny, rt, tlen);
        json_str(stdout, rp >= 0 ? prod : "");
        printf(",\"full_text\":");
        json_str(stdout, rb >= 0 ? big : "");
        printf(",\"health\":");
        print_health_json(stdout, &w);
        printf("}\n");
        CHECK(rp == rb && rp > 0 && strcmp(prod, big) == 0, "worst case does not fit the 1536-B CLI buffer");
        CHECK(rt == -1, "small buffer must refuse (-1), got %d", rt);
    }

    /* I5: TELEMETRY builder with the input device_commands filled, maxed out. */
    for (int adv = 0; adv < 2; adv++) {
        payload_v3_telemetry_input_t w = g_cap;
        evlog_health_t eh;
        worst_health(&eh, INT64_MAX);
        /* device_commands.c emit_status_event mapping (health → input). */
        w.evq_valid = true;
        w.evq_pending_exact = eh.pending_exact;
        w.evq_storage_blocked = eh.storage_blocked;
        w.evq_pending = eh.pending;
        w.evq_deliverable_pending = eh.deliverable_pending;
        w.evq_flash_pending = eh.flash_pending;
        w.evq_sd_pending = eh.sd_pending;
        w.evq_reimport_pending = eh.reimport_pending;
        w.evq_sd_state = event_log_sd_state_name(eh.sd_state);
        w.evq_head_block = event_log_block_name(eh.head_block);
        w.evq_blocked_reason = event_log_blocked_reason_name(eh.blocked_reason);
        w.evq_corrupt_medium = event_log_medium_name(eh.corrupt_medium);
        w.evq_refused_full = eh.refused_full;
        w.evq_refused_media = eh.refused_media;
        w.evq_refused_too_large = eh.refused_too_large;
        w.evq_refused_unavailable = eh.refused_unavailable;
        w.evq_quarantined_poison = eh.quarantined_poison;
        w.evq_quarantined_malformed = eh.quarantined_malformed;
        w.evq_skipped_unindexed_gap = eh.skipped_unindexed_gap;
        w.evq_corrupt_detected = eh.corrupt_detected;
        w.evq_spool_files = eh.spool_files;
        w.evq_spool_errors = eh.spool_errors;
        w.evq_mirror_used = eh.mirror_used;
        w.evq_reclaimed_files = eh.reclaimed_files;
        w.evq_archived_files = eh.archived_files;
        w.evq_reimported_files = eh.reimported_files;
        w.evq_sd_retired_names = eh.sd_retired_names;
        w.evq_sd_bad_copies = eh.sd_bad_copies;
        /* Every other field at the widest value its producer can hand over. */
        static char dev[18], disc[24], wd[16], fw[32], src[16], sha[65], ver[32], bld[32], ins[32];
        static char chn[4][12], sid[4][18], afw[4][16], anm[4][20];
        char c = adv ? '\x01' : 'W';           /* adversarial: every char needs \u00XX */
        fill_str(dev, 17, c); fill_str(disc, 23, c); fill_str(wd, 15, c); fill_str(fw, 31, c);
        fill_str(src, 15, c); fill_str(sha, 64, c); fill_str(ver, 31, c); fill_str(bld, 31, c); fill_str(ins, 31, c);
        w.measure_id = INT64_MAX; w.device = dev; w.observed_utc_ms = INT64_MIN;
        w.observations_valid = true;
        w.air_temperature = -FLT_MAX; w.relative_humidity = -FLT_MAX; w.air_pressure = -FLT_MAX;
        w.connectivity_valid = true; w.wifi = w.provisioned = w.publish_gate = true;
        w.mqtt_reconnects = UINT32_MAX; w.last_disc_reason = disc; w.conn_age_s = INT64_MIN;
        w.pending = INT64_MIN; w.publish_refused = UINT32_MAX; w.last_puback_reason = INT_MIN; w.refusal_hold_s = INT64_MIN;
        w.power_valid = true; w.battery_v = w.input_v = w.system_v = 65.535; w.input_ma = w.charge_ma = 65535;
        w.input_present = true; w.charge_status = 255;
        w.storage_db_valid = w.db_online = true; w.storage_sd_valid = true; w.sd_free_kb = UINT64_MAX;
        w.sd_skipped = w.sd_dropped = w.last_acked_id = INT64_MIN; w.sd_io_lost = true;
        w.runtime_valid = true; w.uptime_s = INT64_MIN;
        w.psram_free_kb = w.psram_largest_kb = w.psram_size_kb = UINT32_MAX;
        w.heap_dma_largest_kb = w.heap_int_free_kb = w.heap_int_largest_kb = UINT32_MAX;
        w.wd_armed = true; w.last_wd_reboot_reason = wd;
        w.clock_valid = true; w.clock_source = src; w.clock_suspect = true;
        w.software_valid = true; w.firmware = fw; w.script_valid = true; w.script_sha256 = sha;
        w.script_version = ver; w.script_built_against_fw = bld; w.script_installed_on_fw = ins; w.script_metadata_verified = true;
        w.attached_count = PAYLOAD_V3_MAX_ATTACHED;
        for (int i = 0; i < (int)PAYLOAD_V3_MAX_ATTACHED; i++) {
            fill_str(chn[i], 11, c); fill_str(sid[i], 17, c); fill_str(afw[i], 15, c); fill_str(anm[i], 19, c);
            w.attached[i] = (payload_v3_attached_sensor_t){ .present = true, .channel = chn[i], .sensor_id = sid[i],
                .firmware = afw[i], .hardware_revision = 255, .name = anm[i], .cal_version_present = true,
                .cal_version = UINT32_MAX };
        }
        char *buf = malloc(PAYLOAD_V3_TELEMETRY_CAP);
        char *big = malloc(1 << 16);
        char err[96] = "", errb[96] = "";
        bool ok = payload_v3_build_telemetry(buf, PAYLOAD_V3_TELEMETRY_CAP, &w, err, sizeof err);
        bool okb = payload_v3_build_telemetry(big, 1 << 16, &w, errb, sizeof errb);
        size_t len = okb ? strlen(big) : 0;
        printf("WORST_TELEMETRY_JSON={\"variant\":\"%s\",\"cap\":%u,\"fits\":%s,\"worst_len\":%zu,\"error\":",
               adv ? "adversarial_escapes" : "max_width", (unsigned)PAYLOAD_V3_TELEMETRY_CAP, ok ? "true" : "false", len);
        json_str(stdout, err);
        printf(",\"payload\":%s}\n", ok ? buf : "null");
        CHECK(okb, "worst-case telemetry did not build even at 64 KiB: %s", errb);
        CHECK(ok ? (strcmp(buf, big) == 0) : (len >= PAYLOAD_V3_TELEMETRY_CAP), "builder truncated or failed with room");
        free(buf); free(big);
    }

    /* Real emit path with adversarial port strings: publishes parseable JSON or fails closed. */
    fill_str(g_disc_reason, 23, '\x02');
    g_wd_reason_forced = true; fill_str(g_wd_reason_override, 15, '\x02');
    fill_str(h_media.app_version, 31, '\x02');
    fill_str(g_script.sha256, 64, '\x02'); fill_str(g_script.version, 31, '\x02');
    fill_str(g_script.built_against_fw, 31, '\x02'); fill_str(g_script.installed_on_fw, 31, '\x02');
    static char csrc[16];
    fill_str(csrc, 15, '\x02');
    h_set_clock_source(csrc);
    g_env[0] = g_env[1] = g_env[2] = -FLT_MAX;
    g_power = (power_reading_t){ 65535, 65535, 65535, 65535, 65535, true, 255 };
    size_t before = g_npub;
    B.connected = true;
    cmd_result_t r = cmd_publish_status_event();
    B.connected = false;
    const char *env = NULL;
    for (size_t i = before; i < g_npub; i++) if (g_pub[i].msg_id == 0) env = g_pub[i].env;
    printf("WORST_EMIT_JSON={\"status\":\"%s\",\"published\":%s,\"message\":", esp_err_to_name(r.status), env ? "true" : "false");
    json_str(stdout, r.message);
    printf(",\"envelope\":%s}\n", env ? env : "null");
    CHECK((r.status == ESP_OK && env != NULL) || (r.status == ESP_ERR_INVALID_SIZE && env == NULL), "emit neither published nor failed closed");
    ev_common();
}

/* ── I6: keeper task on the integration clock vs. a direct deterministic run ── */

static void print_tree(const char *dir, const char *label)
{
    DIR *d = opendir(dir);
    if (d == NULL) return;
    char names[4096][64];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nn < 4096) {
        if (e->d_name[0] == '.') continue;
        snprintf(names[nn++], 64, "%s", e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)nn, sizeof names[0], (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i < nn; i++) {
        char p[256];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        FILE *f = fopen(p, "rb");
        if (f == NULL) continue;
        uint32_t crc = 0;
        size_t n, total = 0;
        static char buf[65536];
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) { crc = evq_crc32(crc, buf, n); total += n; }
        fclose(f);
        printf("I6_FILE=%s/%s %zu %08x\n", label, names[i], total, crc);
    }
}

static void sc_I6(bool task_mode)
{
    g_scenario = "I6";              /* identical workload bytes in both modes */
    boot(&(boot_t){ .card = true, .keeper = task_mode });
    uint64_t last_service_us = 0;
    if (!task_mode) { while (event_log_sd_service_pending()) direct_service(); last_service_us = h_vt_us(); }
    for (int i = 0; i < 700; i++) {
        esp_err_t e = produce();
        CHECK(e == ESP_OK, "I6 store %d: %s", i, esp_err_to_name(e));
        /* The direct run services exactly where the keeper would: on a pending
         * notification, or when its production period elapsed. */
        if (!task_mode) {
            if (event_log_sd_service_pending() || h_vt_us() - last_service_us >= (uint64_t)EVQ_KEEPER_PERIOD_MS * 1000) {
                while (event_log_sd_service_pending() || h_vt_us() - last_service_us >= (uint64_t)EVQ_KEEPER_PERIOD_MS * 1000) {
                    direct_service();
                    last_service_us = h_vt_us();
                }
            }
        }
        wait_ms(2000);
        if (!task_mode) {
            while (event_log_sd_service_pending() || h_vt_us() - last_service_us >= (uint64_t)EVQ_KEEPER_PERIOD_MS * 1000) {
                direct_service();
                last_service_us = h_vt_us();
            }
        }
    }
    evlog_health_t h = health();
    EV("I6_COUNTS", "{\"spool_files\":%u,\"reclaimed_files\":%u,\"archived_files\":%u,\"sd_bursts\":%u,"
                    "\"pressure_notifies\":%u,\"spool_errors\":%u,\"mirror_used\":%u,\"index_segments\":%u,"
                    "\"pending\":%lld,\"sd_pending\":%lld,\"flash_pending\":%lld,\"rd_seq\":%u,\"tail_seq\":%u}",
       h.spool_files, h.reclaimed_files, h.archived_files, h.sd_bursts, h.pressure_notifies, h.spool_errors,
       h.mirror_used, h.index_segments, (long long)h.pending, (long long)h.sd_pending, (long long)h.flash_pending,
       (unsigned)h.rd_seq, (unsigned)h.tail_seq);
    evq_index_t ix;
    if (evq_index_init(&ix, 8192) == ESP_OK && evq_index_load(&ix, EVSTORE_MOUNT "/evq.idx") == ESP_OK) {
        for (size_t i = 0; i < ix.n; i++) {
            const evq_seg_t *s = &ix.segs[i];
            printf("I6_INDEX=%u %s %lld %lld %u %u %08x %08x %s %s\n", (unsigned)s->seq, evq_seg_state_name(s->state),
                   (long long)s->first_id, (long long)s->last_id, s->count, s->bytes, (unsigned)s->crc,
                   (unsigned)s->cid, s->primary, s->mirror);
        }
        evq_index_free(&ix);
    }
    print_tree("evstore/events", "flash");
    print_tree("sdcard/events", "sd_events");
    print_tree("sdcard/evq", "sd_evq");
    print_tree("sdcard/archive", "sd_archive");
    CHECK(h.spool_files > 0 && h.reclaimed_files > 0, "I6 workload never spooled/reclaimed");
    EV("I6_MODE", "%s", task_mode ? "keeper_task" : "direct");
    ev_common();
}

/* ── I7: automatic delivery through the production tasks only ── */

static void sensor_task(void *arg)
{
    (void)arg;
    char resp[16]; size_t len = 0;
    cmd_result_t r = cmd_uart_text_query(1, "get_temp", "\n", 10000, resp, sizeof resp, &len);
    h_trace("sensor", r.status == ESP_OK ? "done" : "fail", 0);
    for (;;) vTaskDelay(portMAX_DELAY);
}

static void sc_I7(void)
{
    g_trace_claims = true;
    boot(&(boot_t){ .card = true, .keeper = true, .sync_runner = true, .power_port = true });
    g_input_present = false;                  /* on battery: upload gate closed */
    build_backlog(3ULL * h_media.flash_total / 2, 3000, 0);
    evlog_health_t h0 = health();
    ev_health("I7_BACKLOG", &h0);
    CHECK(h0.sd_pending > 0, "no SD_ONLY part in the backlog");
    CHECK(g_claims == 0, "claims while the gate was closed and MQTT down");
    uint64_t t_open = h_vt_us();
    g_input_present = true;                   /* external power back */
    broker_connect();
    size_t target1 = g_nacc * 2 / 5;
    uint64_t t_closed_req = 0, t_reopen_req = 0, t_hold = 0, claims_ok_at_reopen = 0;
    int phase = 0;
    for (int s = 0; s < 6 * 3600; s++) {
        size_t delivered = 0;
        for (size_t k = 0; k < g_npub; k++) if (g_pub[k].outcome == OUT_OK && g_pub[k].msg_id) delivered++;
        if (phase == 0 && delivered >= target1) { g_input_present = false; t_closed_req = h_vt_us(); phase = 1; }
        else if (phase == 1 && h_vt_us() - t_closed_req >= 6ULL * 60 * 1000000) {
            g_input_present = true; t_reopen_req = h_vt_us(); claims_ok_at_reopen = g_claims_ok; phase = 2;
        }
        else if (phase == 2 && delivered >= g_nacc * 4 / 5 && g_gate_open) {
            g_hold_ms = 3000;
            TaskHandle_t st;
            xTaskCreate(sensor_task, "sensor", 4096, NULL, SENSOR_PRIO, &st);
            t_hold = h_vt_us();
            phase = 3;
        } else if (phase == 3 && s % 3 == 0 && g_nacc < 600) {
            produce();
        }
        if (phase == 3 && delivered_all() && h_vt_us() - t_hold > 60ULL * 1000000) break;
        /* No stores from the close request until delivery resumed after the
         * reopen: the gate opening is not a store event, so only the
         * production fallback wake can find it. */
        bool resumed = phase == 2 && g_claims_ok > claims_ok_at_reopen;
        if ((phase == 0 || resumed) && s % 3 == 0) produce();
        wait_ms(1000);
    }
    CHECK(phase == 3, "I7 did not reach every phase (phase=%d)", phase);
    CHECK(delivered_all(), "I7 did not finish");
    /* Trace analysis. */
    uint64_t drains = 0, drains_other_task = 0, drains_without_wake = 0, wn = 0, wt = 0;
    uint64_t claims_closed = 0, claims_hold = 0, claims_after_reopen = 0;
    bool gate_open = false, wake_since_drain = false;
    uint64_t t_gate_closed = 0, t_gate_open2 = 0;
    int gate_opens = 0, gate_closes = 0;
    uint64_t t_sensor_done = 0;
    for (size_t i = 0; i < h_trace_count(); i++) {
        const h_trace_t *e = h_trace_at(i);
        if (strcmp(e->kind, "gate") == 0) {
            gate_open = e->n != 0;
            if (gate_open) { gate_opens++; if (gate_closes > 0 && !t_gate_open2) t_gate_open2 = e->vt_us; }
            else { gate_closes++; if (!t_gate_closed) t_gate_closed = e->vt_us; }
        } else if (strcmp(e->kind, "sensor") == 0) {
            t_sensor_done = e->vt_us;
        } else if (strcmp(e->kind, "wake") == 0 && strcmp(e->task, "sync_runner") == 0) {
            wake_since_drain = true;
            if (strcmp(e->a, "notify") == 0) wn++; else wt++;
        } else if (strcmp(e->kind, "drain_enter") == 0) {
            drains++;
            if (strcmp(e->task, "sync_runner") != 0) drains_other_task++;
            if (!wake_since_drain) drains_without_wake++;
            wake_since_drain = false;
        } else if (strcmp(e->kind, "claim") == 0) {
            if (!gate_open) claims_closed++;
            if (t_gate_open2 && e->vt_us >= t_gate_open2) claims_after_reopen++;
            if (t_hold && e->vt_us >= t_hold && e->vt_us < t_sensor_done) claims_hold++;
        }
    }
    EV("I7_TRACE", "{\"drain_passes\":%llu,\"drain_passes_outside_sync_runner\":%llu,\"drain_passes_without_wake\":%llu,"
                   "\"wakes_notify\":%llu,\"wakes_fallback\":%llu,\"gate_opens\":%d,\"gate_closes\":%d,"
                   "\"claims_while_gate_closed\":%llu,\"claims_during_sensor_hold\":%llu,\"claims_after_reopen\":%llu,"
                   "\"claims_gate_closed_port_view\":%llu,\"t_open_request_ms\":%llu,\"t_close_request_ms\":%llu,"
                   "\"t_gate_closed_ms\":%llu,\"t_reopen_request_ms\":%llu,\"t_gate_reopened_ms\":%llu,\"t_hold_ms\":%llu,\"t_hold_end_ms\":%llu}",
       (unsigned long long)drains, (unsigned long long)drains_other_task, (unsigned long long)drains_without_wake,
       (unsigned long long)wn, (unsigned long long)wt, gate_opens, gate_closes,
       (unsigned long long)claims_closed, (unsigned long long)claims_hold, (unsigned long long)claims_after_reopen,
       (unsigned long long)g_claims_gate_closed, (unsigned long long)(t_open / 1000),
       (unsigned long long)(t_closed_req / 1000), (unsigned long long)(t_gate_closed / 1000),
       (unsigned long long)(t_reopen_req / 1000), (unsigned long long)(t_gate_open2 / 1000),
       (unsigned long long)(t_hold / 1000), (unsigned long long)(t_sensor_done / 1000));
    CHECK(g_direct_drain_calls == 0 && g_direct_service_calls == 0, "test called drain/service directly");
    CHECK(drains > 0 && drains_other_task == 0 && drains_without_wake == 0, "a drain pass not caused by a wake");
    CHECK(wn > 0 && wt > 0, "both notifier and fallback wakes expected");
    CHECK(gate_opens >= 2 && gate_closes >= 1, "gate never closed/reopened");
    CHECK(claims_closed == 0 && g_claims_gate_closed == 0 && claims_hold == 0, "claims while the gate was closed");
    CHECK(claims_after_reopen > 0, "delivery did not resume after reopen");
    e2e_t r = verify_e2e("I7");
    CHECK(r.missing == 0 && r.mismatched == 0 && r.ts_mismatch == 0 && r.foreign == 0 && r.first_order_ok, "I7 delivery");
    ev_common();
}

/* ── I8: keeper wake from pressure / mount / unpark on its 60-s production period ── */

static void sc_I8(void)
{
    g_size_profile = 1;
    g_fixed_size = 4000;
    g_trace_stores = true;
    boot(&(boot_t){ .card = true, .keeper = true });
    uint32_t pn0 = health().pressure_notifies;
    uint64_t t_pressure = 0;
    size_t tr_pressure = 0;
    int store_idx = 0;
    for (int i = 0; i < 600; i++) {
        size_t tr = h_trace_count();
        CHECK(produce() == ESP_OK, "I8 store");
        store_idx++;
        if (!t_pressure && health().pressure_notifies > pn0) { t_pressure = h_vt_us(); tr_pressure = tr; }
        wait_ms(1000);
        if (t_pressure && h_vt_us() - t_pressure > 5ULL * 1000000) break;
    }
    CHECK(t_pressure != 0, "pressure never crossed");
    /* store → notify → keeper wake → service (SD transfer) at the same virtual instant. */
    uint64_t t_notify = 0, t_wake = 0, t_first_create = 0, t_prev_wake = 0;
    for (size_t i = 0; i < h_trace_count(); i++) {
        const h_trace_t *e = h_trace_at(i);
        if (i < tr_pressure) {
            if (strcmp(e->task, "sd_keeper") == 0 && strcmp(e->kind, "wake") == 0) t_prev_wake = e->vt_us;
            continue;
        }
        if (!t_notify && strcmp(e->kind, "notify") == 0 && strcmp(e->a, "sd_keeper") == 0) t_notify = e->vt_us;
        else if (t_notify && !t_wake && strcmp(e->task, "sd_keeper") == 0 && strcmp(e->kind, "wake") == 0) t_wake = e->vt_us;
        else if (t_wake && !t_first_create && strcmp(e->task, "sd_keeper") == 0 && strcmp(e->kind, "sd_create") == 0) t_first_create = e->vt_us;
    }
    evlog_health_t h1 = health();
    EV("I8_PRESSURE", "{\"store_index\":%d,\"t_store_ms\":%llu,\"t_notify_ms\":%llu,\"t_keeper_wake_ms\":%llu,"
                      "\"t_first_sd_create_ms\":%llu,\"t_prev_keeper_wake_ms\":%llu,\"next_periodic_ms\":%llu,"
                      "\"spool_files_after\":%u,\"keeper_period_ms\":%u}",
       store_idx, (unsigned long long)(t_pressure / 1000), (unsigned long long)(t_notify / 1000),
       (unsigned long long)(t_wake / 1000), (unsigned long long)(t_first_create / 1000),
       (unsigned long long)(t_prev_wake / 1000), (unsigned long long)((t_prev_wake / 1000) + EVQ_KEEPER_PERIOD_MS),
       h1.spool_files, (unsigned)EVQ_KEEPER_PERIOD_MS);
    CHECK(t_notify == t_pressure && t_wake == t_pressure && t_first_create == t_pressure,
          "pressure wake not within one scheduler turn");
    CHECK(t_prev_wake / 1000 + EVQ_KEEPER_PERIOD_MS > t_pressure / 1000 + 1000, "wake coincided with the period");
    CHECK(h1.spool_files > 0, "spooling did not start");

    /* Mount wake (app_on_sd_state_change → event_log_sd_notify). */
    card_remove();
    wait_ms(7300);
    size_t trm = h_trace_count();
    uint64_t t_mount = h_vt_us();
    card_insert(CID_A);
    wait_ms(500);
    uint64_t t_mount_wake = 0;
    for (size_t i = trm; i < h_trace_count(); i++) {
        const h_trace_t *e = h_trace_at(i);
        if (!t_mount_wake && strcmp(e->task, "sd_keeper") == 0 && strcmp(e->kind, "wake") == 0 && strcmp(e->a, "notify") == 0) t_mount_wake = e->vt_us;
    }
    /* Unpark wake (event_log_set_sd_parked(false)). */
    event_log_set_sd_parked(true);
    CHECK(sdcard_unmount() == ESP_OK, "park unmount");
    wait_ms(11700);
    size_t tru = h_trace_count();
    CHECK(sdcard_mount() == ESP_OK, "unpark mount");
    uint64_t t_unpark = h_vt_us();
    event_log_set_sd_parked(false);
    wait_ms(500);
    uint64_t t_unpark_wake = 0;
    for (size_t i = tru; i < h_trace_count(); i++) {
        const h_trace_t *e = h_trace_at(i);
        if (!t_unpark_wake && strcmp(e->task, "sd_keeper") == 0 && strcmp(e->kind, "wake") == 0 && strcmp(e->a, "notify") == 0) t_unpark_wake = e->vt_us;
    }
    EV("I8_MOUNT_UNPARK", "{\"t_mount_ms\":%llu,\"t_mount_wake_ms\":%llu,\"t_unpark_ms\":%llu,\"t_unpark_wake_ms\":%llu}",
       (unsigned long long)(t_mount / 1000), (unsigned long long)(t_mount_wake / 1000),
       (unsigned long long)(t_unpark / 1000), (unsigned long long)(t_unpark_wake / 1000));
    CHECK(t_mount_wake == t_mount, "mount did not wake the keeper immediately");
    CHECK(t_unpark_wake == t_unpark, "unpark did not wake the keeper immediately");
    CHECK(g_direct_service_calls == 0, "manual service call");
    /* The whole keeper wake trace, for the evidence. */
    for (size_t i = 0; i < h_trace_count(); i++) {
        const h_trace_t *e = h_trace_at(i);
        bool around = e->vt_us + 2000000 >= t_pressure && e->vt_us <= t_pressure + 1000000;
        bool keep = (strcmp(e->task, "sd_keeper") == 0 && strcmp(e->kind, "wake") == 0) ||
                    (strcmp(e->kind, "notify") == 0 && strcmp(e->a, "sd_keeper") == 0) ||
                    strcmp(e->kind, "card") == 0 ||
                    (around && (strcmp(e->kind, "store") == 0 || strcmp(e->kind, "store_ret") == 0 ||
                                (strcmp(e->task, "sd_keeper") == 0 && strcmp(e->kind, "sd_create") == 0)));
        if (keep) printf("I8_TRACE=%llu %s %s %s %lld\n", (unsigned long long)(e->vt_us / 1000), e->task, e->kind, e->a, (long long)e->n);
    }
    ev_common();
}

/* ── Q1 (publisher half): pre-upgrade oversize record → publish-cap quarantine ── */

static void sc_Q1(void)
{
    /* Pre-upgrade flash tail, written raw BEFORE the first event_log_init. */
    mkdir("evstore", 0777);
    mkdir("evstore/events", 0777);
    FILE *f = fopen("evstore/events/ev-000001.log", "w");
    size_t plen = 20000;
    char *pl = malloc(plen + 1);
    int hn = snprintf(pl, plen, "{\"synthetic_test\":true,\"fixture\":\"q1_oversize\",\"blob\":\"");
    memset(pl + hn, 'q', plen - (size_t)hn - 2);
    pl[plen - 2] = '"'; pl[plen - 1] = '}'; pl[plen] = '\0';
    char *line = malloc(plen + 256);
    int ln = snprintf(line, plen + 256, "4242\tuart_1\tambit\tMEASUREMENT\tarrun 1\t1790379000000\t1790379001000\t\t%s\n", pl);
    fwrite(line, 1, (size_t)ln, f);
    fclose(f);
    boot(&(boot_t){ .card = true, .keeper = true });
    g_size_profile = 2;            /* neighbours' envelopes stay under the lowered cap */
    for (int i = 0; i < 30; i++) { CHECK(produce() == ESP_OK, "Q1 store"); wait_ms(1000); }
    evlog_health_t h0 = health();
    uint32_t s0, o0;
    cursor(&s0, &o0);
    size_t tr0 = h_trace_count();
    broker_connect();
    wait_ms(100);
    drain_direct_until_empty(20000);
    evlog_health_t h1 = health();
    uint32_t s1, o1;
    cursor(&s1, &o1);
    /* Quarantine content must be the fixture's intact line. */
    FILE *q = fopen("evstore/events/quarantine.log", "rb");
    char *qb = malloc((size_t)ln + 1024);
    size_t qn = q ? fread(qb, 1, (size_t)ln + 1024, q) : 0;
    if (q) fclose(q);
    bool q_intact = qn == (size_t)ln && memcmp(qb, line, (size_t)ln) == 0;
    /* Order: quarantine fsync precedes the first cursor persistence past the fixture. */
    uint64_t t_q = 0; size_t i_q = 0, i_cur = 0;
    for (size_t i = tr0; i < h_trace_count(); i++) {
        const h_trace_t *e = h_trace_at(i);
        if (!i_q && strcmp(e->kind, "fsync_quarantine") == 0) { i_q = i; t_q = e->vt_us; }
        if (!i_cur && strcmp(e->kind, "nvs_cursor") == 0 && (e->n % 100000000LL) >= ln && e->n / 100000000LL == 1) i_cur = i;
    }
    bool fixture_published = false;
    for (size_t k = 0; k < g_npub; k++) if (g_pub[k].mid == 4242) fixture_published = true;
    printf("Q1_OVERSIZE={\"fixture_line_bytes\":%d,\"publish_cap\":%u,\"quarantine_bytes\":%zu,\"quarantine_intact\":%s,"
           "\"fsync_quarantine_trace_idx\":%zu,\"first_cursor_past_fixture_trace_idx\":%zu,\"cursor_before\":\"%u:%u\","
           "\"cursor_after\":\"%u:%u\",\"quarantined_poison_before\":%lld,\"quarantined_poison_after\":%lld,"
           "\"oversize_skipped_from_message\":%u,\"fixture_published\":%s,\"t_quarantine_ms\":%llu,\"message\":",
           ln, (unsigned)AMBYTE_PUBLISH_MAX_BYTES, qn, q_intact ? "true" : "false", i_q, i_cur, (unsigned)s0, (unsigned)o0,
           (unsigned)s1, (unsigned)o1, (long long)h0.quarantined_poison, (long long)h1.quarantined_poison,
           g_oversize_from_msg, fixture_published ? "true" : "false", (unsigned long long)(t_q / 1000));
    json_str(stdout, g_last_oversize_msg);
    printf("}\n");
    CHECK(s0 == 1 && o0 == 0, "cursor must start at the fixture");
    CHECK(q_intact, "quarantine.log does not hold the fixture's intact bytes");
    CHECK(i_q > 0 && i_cur > i_q, "cursor persisted past the fixture before the quarantine fsync");
    CHECK(h1.quarantined_poison == h0.quarantined_poison + 1, "quarantined_poison must rise by exactly 1");
    CHECK(g_oversize_from_msg == 1, "oversize_skipped must be exactly 1");
    CHECK(!fixture_published, "the fixture was published");
    g_allowed_foreign = NULL;
    e2e_t r = verify_e2e("Q1");
    CHECK(r.missing == 0 && r.mismatched == 0 && r.ts_mismatch == 0 && r.foreign == 0 && r.first_order_ok && r.duplicates == 0,
          "Q1 neighbours");
    free(pl); free(line); free(qb);
    ev_common();
}


/* Carry the accepted set across a forked "reboot" (one record per line; no
 * field contains a tab or newline). */
static void save_accepted(const char *path)
{
    FILE *f = fopen(path, "w");
    for (size_t i = 0; f && i < g_nacc; i++) {
        rec_t *a = &g_acc[i];
        fprintf(f, "%lld\t%lld\t%lld\t%d\t%s\t%s\t%s\n", (long long)a->id, (long long)a->start_ms,
                (long long)a->end_ms, a->v3 ? 1 : 0, a->cmd, a->meta ? a->meta : "-", a->payload);
    }
    if (f) fclose(f);
}

static void load_accepted(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return;
    static char line[70000];
    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = '\0';
        char *fld[7]; int k = 0;
        for (char *p = line; k < 7; ) { fld[k++] = p; char *t = strchr(p, '\t'); if (!t) break; *t = '\0'; p = t + 1; }
        if (k != 7) continue;
        if (g_nacc == g_capacc) {
            g_capacc = g_capacc ? g_capacc * 2 : 1024;
            g_acc = realloc(g_acc, g_capacc * sizeof *g_acc);
            if (g_acc == NULL) abort();
        }
        rec_t *a = &g_acc[g_nacc++];
        a->id = strtoll(fld[0], NULL, 10); a->start_ms = strtoll(fld[1], NULL, 10);
        a->end_ms = strtoll(fld[2], NULL, 10); a->v3 = fld[3][0] == '1';
        a->cmd = xstrdup(fld[4]); a->meta = strcmp(fld[5], "-") ? xstrdup(fld[5]) : NULL;
        a->payload = xstrdup(fld[6]); a->line_len = strlen(a->payload) + 60;
    }
    fclose(f);
}

/* Finding 2 (fixed): the card's copies are modified while it is out, and the
 * remove -> reinsert of the SAME card happens with no event_log observation in
 * between. Each transition goes through event_log_sd_notify() exactly as
 * app_main's monitor callback (app_on_sd_state_change) does, and nothing else.
 * both=false: only primaries altered -> the mirror must be used, full R-E2E.
 * both=true:  primary and mirror altered -> the head must wait (backlog_corrupt).
 * Either way, not one altered byte may be published. */
static void sc_epoch(bool both)
{
    boot(&(boot_t){ .card = true, .keeper = true });
    outage_to_sd_only();
    evlog_health_t h0 = health();
    card_remove();                            /* monitor callback -> event_log_sd_notify */
    char dir[64];
    snprintf(dir, sizeof dir, "cards/%08x/events", (unsigned)CID_A);
    int n = corrupt_files_in(dir);
    if (both) { snprintf(dir, sizeof dir, "cards/%08x/evq", (unsigned)CID_A); n += corrupt_files_in(dir); }
    card_insert(CID_A);                       /* monitor callback -> event_log_sd_notify */
    broker_connect();
    wait_ms(100);
    if (both) {
        for (int i = 0; i < 200; i++) { direct_drain(); wait_ms(1000); }
    } else {
        drain_direct_until_empty(20000);
    }
    evlog_health_t h1 = health();
    ev_health("EPOCH_BEFORE", &h0);
    ev_health("EPOCH_AFTER", &h1);
    e2e_t r = verify_e2e("EPOCH");
    EV("EPOCH_RESULT", "{\"variant\":\"%s\",\"altered_files\":%d,\"sd_pending_before\":%lld,"
                       "\"delivered_with_altered_bytes\":%zu,\"mirror_used\":%u,\"delivered\":%zu,\"accepted\":%zu}",
       both ? "primary_and_mirror" : "primary_only", n, (long long)h0.sd_pending, r.mismatched,
       h1.mirror_used, r.delivered, r.accepted);
    CHECK(n > 0, "no SD copy was altered");
    CHECK(r.mismatched == 0, "altered SD bytes were published (%zu records)", r.mismatched);
    CHECK(r.ts_mismatch == 0 && r.foreign == 0 && (both || r.first_order_ok), "order/timestamps");
    if (both) {
        CHECK(r.delivered == 0 && r.missing == r.accepted, "records delivered past a corrupt head");
        CHECK(strcmp(event_log_sd_state_name(h1.sd_state), "backlog_corrupt") == 0 &&
              strcmp(event_log_medium_name(h1.corrupt_medium), "sd") == 0 && h1.deliverable_pending == 0,
              "head must wait with sd_state=backlog_corrupt");
    } else {
        CHECK(r.missing == 0 && r.duplicates == 0, "mirror fallback must deliver everything");
        CHECK(h1.mirror_used > 0, "mirror never used");
    }
    ev_common();
}

/* Finding 1 (fixed): flash-only files leave the queue through a foreign
 * (rollback) cursor move -> REIMPORT entries whose ONLY copy is on flash, with
 * less than the re-import room free. Filling the store must not evict them;
 * once delivery frees space they are re-imported and delivered. */
static void sc_reimport_evict(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        boot(&(boot_t){ .card = false, .keeper = false });
        for (int i = 0; i < 120; i++) { produce(); wait_ms(1000); }
        uint32_t seq = 0, off = 0, tail = 0;
        (void)event_log_cursor_info(&seq, &off, &tail);
        (void)event_log_prepare_shutdown();
        save_accepted("boot1_accepted.tsv");
        FILE *f = fopen("tail.txt", "w");
        if (f) { fprintf(f, "%u %zu\n", (unsigned)tail, g_nacc); fclose(f); }
        fflush(stdout);
        _exit(g_failures == 0 ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "boot-1 child failed");
    unsigned tail = 0; size_t n1 = 0;
    FILE *f = fopen("tail.txt", "r");
    if (f) { if (fscanf(f, "%u %zu", &tail, &n1) != 2) tail = 0; fclose(f); }
    h_nvs_set_path("nvs.dat");
    nvs_handle_t h;
    (void)nvs_open("evlog", NVS_READWRITE, &h);
    nvs_set_u32(h, "rd_seq", tail);           /* what a rolled-back firmware writes */
    nvs_set_u32(h, "rd_off", 0);
    nvs_commit(h);
    load_accepted("boot1_accepted.tsv");
    CHECK(g_nacc == n1 && n1 > 0, "boot-1 accepted set not carried over");
    boot(&(boot_t){ .card = false, .keeper = true });
    wait_ms(1000);
    evlog_health_t a = health();
    for (int i = 0; i < 400 && !health().storage_blocked; i++) { produce(); wait_ms(500); }
    evlog_health_t b = health();
    CHECK(b.storage_blocked, "store never filled");
    /* reimport_pending may only fall by records actually re-imported. */
    bool dropped_without_reimport = b.reimport_pending < a.reimport_pending && b.reimported_files == a.reimported_files;
    broker_connect();
    wait_ms(100);
    uint64_t t0 = h_vt_us();
    bool done = false;
    for (int i = 0; i < 6 * 3600 && !done; i++) {
        direct_drain();
        wait_ms(1000);
        evlog_health_t c = health();
        done = c.pending == 0 && c.reimport_pending == 0 && delivered_all();
    }
    evlog_health_t z = health();
    uint64_t free_end = 0;
    (void)event_log_free_bytes(&free_end);
    EV("REIMPORT_ROOM", "{\"flash_total\":%zu,\"flash_free_end\":%llu,\"reimport_needs_free_pct\":%u,"
                        "\"free_pct_end\":%llu}",
       h_media.flash_total, (unsigned long long)free_end, (unsigned)EVQ_RECLAIM_PCT,
       (unsigned long long)(free_end * 100 / h_media.flash_total));
    EV("REIMPORT_EVICT", "{\"boot1_accepted\":%zu,\"reimport_pending_after_boot\":%lld,\"reimport_pending_after_fill\":%lld,"
                         "\"reimported_files_after_fill\":%u,\"dropped_without_reimport\":%s,\"reimported_files_end\":%u,"
                         "\"reimport_pending_end\":%lld,\"pending_end\":%lld,\"drain_ms\":%llu,\"finished\":%s}",
       n1, (long long)a.reimport_pending, (long long)b.reimport_pending, b.reimported_files,
       dropped_without_reimport ? "true" : "false", z.reimported_files, (long long)z.reimport_pending,
       (long long)z.pending, (unsigned long long)((h_vt_us() - t0) / 1000), done ? "true" : "false");
    ev_health("REIMPORT_AFTER_BOOT", &a);
    ev_health("REIMPORT_AFTER_FILL", &b);
    /* The keeper wakes at boot, so the obligations may already be re-imported
     * by the first sample: either way the foreign move must have created them. */
    CHECK(a.reimport_pending > 0 || a.reimported_files > 0, "the foreign cursor move produced no REIMPORT obligation");
    CHECK(!dropped_without_reimport, "reimport_pending dropped with no re-import (evicted)");
    CHECK(done, "the queue did not drain the re-import obligations");
    e2e_t r = verify_e2e("REIMPORT");
    CHECK(r.missing == 0 && r.mismatched == 0 && r.ts_mismatch == 0 && r.foreign == 0, "R-E2E over boot1 + boot2");
    ev_common();
}

/* Finding 4 (fixed): with the RAM index at cap, a rotation must not write an S
 * line that RAM refused. Boot 1 rotates well past the cap; its live RAM index
 * (health) must equal the on-disk fold, and after a reboot the replayed index
 * must equal the pre-reboot one with no rejected line. */
static void print_disk_fold(const char *label, size_t cap)
{
    evq_index_t ix;
    if (evq_index_init(&ix, cap) != ESP_OK) return;
    esp_err_t e = evq_index_load(&ix, EVSTORE_MOUNT "/evq.idx");
    printf("%s={\"cap\":%zu,\"load\":\"%s\",\"n\":%zu,\"lines\":%u,\"bad_lines\":%u,\"segs\":\"", label, cap,
           esp_err_to_name(e), ix.n, (unsigned)ix.lines, (unsigned)ix.bad_lines);
    for (size_t i = 0; i < ix.n; i++) printf("%s%u:%s", i ? "," : "", (unsigned)ix.segs[i].seq, evq_seg_state_name(ix.segs[i].state));
    printf("\"}\n");
    evq_index_free(&ix);
}

static void sc_index_cap_replay(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        boot(&(boot_t){ .card = true, .keeper = true });
        for (int i = 0; i < 600; i++) {
            produce();
            wait_ms(1000);
            uint32_t tail = 0;
            (void)event_log_cursor_info(NULL, NULL, &tail);
            if (tail > 3 * EVQ_INDEX_CAP) break;
        }
        evlog_health_t h = health();
        ev_health("IDXCAP_BOOT1", &h);
        print_disk_fold("IDXCAP_DISK_BOOT1", 8192);
        CHECK(event_log_prepare_shutdown() == ESP_OK, "shutdown");
        fflush(stdout);
        _exit(g_failures == 0 ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "boot-1 child failed");
    print_disk_fold("IDXCAP_DISK_BEFORE_REBOOT", 8192);
    print_disk_fold("IDXCAP_DISK_AT_CAP", EVQ_INDEX_CAP);
    boot(&(boot_t){ .card = true, .keeper = false });
    evlog_health_t h2 = health();
    ev_health("IDXCAP_BOOT2", &h2);
    ev_common();
}

static void *volatile g_leak_sink;

/* ════════════════════════ main ══════════════════════════════════════════ */

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s <scenario> [seed]\n", argv[0]); return 2; }
    g_scenario = argv[1];
    if (argc >= 3) g_seed = (unsigned)strtoul(argv[2], NULL, 10);
    g_rng ^= (uint64_t)g_seed * 0x2545F4914F6CDD1DULL;
    for (int i = 0; i < 8; i++) (void)rnd();
    EV("SCENARIO", "%s", g_scenario);
    EV("SEED", "%u", g_seed);
    EV("PARAMS", "{\"flash_total\":%zu,\"rotate\":%u,\"min_free\":%u,\"archive_every\":%u,\"pressure_pct\":%u,"
                 "\"reclaim_pct\":%u,\"sd_reserve\":%llu,\"keeper_period_ms\":%u,\"index_cap\":%u,\"publish_max\":%u,"
                 "\"telemetry_cap\":%u,\"window_slots\":%u,\"window_bytes\":%u}",
       h_media.flash_total, (unsigned)EVLOG_ROTATE_BYTES, (unsigned)EVLOG_MIN_FREE_BYTES, (unsigned)EVLOG_ARCHIVE_EVERY_N,
       (unsigned)EVQ_PRESSURE_PCT, (unsigned)EVQ_RECLAIM_PCT, (unsigned long long)EVQ_SD_RESERVE_BYTES,
       (unsigned)EVQ_KEEPER_PERIOD_MS, (unsigned)EVQ_INDEX_CAP, (unsigned)AMBYTE_PUBLISH_MAX_BYTES,
       (unsigned)PAYLOAD_V3_TELEMETRY_CAP, (unsigned)PUBLISH_WINDOW_SLOTS, (unsigned)PUBLISH_WINDOW_BYTES);
    const char *s = g_scenario;
    if (!strcmp(s, "I1")) sc_I1();
    else if (!strcmp(s, "I2")) sc_I2();
    else if (!strcmp(s, "I3")) sc_I3();
    else if (!strcmp(s, "I4")) sc_I4();
    else if (!strcmp(s, "I6_task")) sc_I6(true);
    else if (!strcmp(s, "I6_direct")) sc_I6(false);
    else if (!strcmp(s, "I7")) sc_I7();
    else if (!strcmp(s, "I8")) sc_I8();
    else if (!strcmp(s, "Q1_oversize")) sc_Q1();
    else if (!strcmp(s, "worst")) sc_worst();
    else if (!strcmp(s, "st_normal")) st_normal();
    else if (!strcmp(s, "st_blocked_sd_unavailable")) st_blocked_sd_unavailable();
    else if (!strcmp(s, "st_blocked_sd_full")) st_blocked_sd_full();
    else if (!strcmp(s, "st_blocked_sd_error")) st_blocked_sd_error();
    else if (!strcmp(s, "st_blocked_sd_mismatch")) st_blocked_sd_mismatch();
    else if (!strcmp(s, "st_sd_mismatch_head")) st_sd_mismatch_head();
    else if (!strcmp(s, "reimport_evict")) sc_reimport_evict();
    else if (!strcmp(s, "epoch_primary")) sc_epoch(false);
    else if (!strcmp(s, "epoch_both")) sc_epoch(true);
    else if (!strcmp(s, "index_cap_replay")) sc_index_cap_replay();
    else if (!strcmp(s, "canary_asan")) { char *p = malloc(8); free(p); volatile char c = p[g_seed % 8]; (void)c; }
    else if (!strcmp(s, "canary_ubsan")) { volatile int x = INT_MAX; x = x + (int)g_seed; EV("X", "%d", x); }
    else if (!strcmp(s, "canary_lsan")) { g_leak_sink = malloc(4096); g_leak_sink = NULL; }
    else if (!strcmp(s, "st_blocked_backlog_waiting")) st_blocked_backlog_waiting();
    else if (!strcmp(s, "st_blocked_index_cap")) st_blocked_index_cap();
    else if (!strcmp(s, "st_blocked_transfer_pending")) st_blocked_transfer_pending();
    else if (!strcmp(s, "st_sd_lost_parked")) st_sd_lost_parked();
    else if (!strcmp(s, "st_backlog_corrupt")) st_backlog_corrupt();
    else if (!strcmp(s, "st_corrupt_flash")) st_corrupt_flash();
    else if (!strcmp(s, "st_refused")) st_refused();
    else if (!strcmp(s, "st_unavailable")) st_unavailable();
    else if (!strcmp(s, "st_quarantined")) st_quarantined();
    else if (!strcmp(s, "st_upgrade")) st_upgrade();
    else { fprintf(stderr, "unknown scenario %s\n", s); return 2; }
    EV("FAILURES", "%d", g_failures);
    fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
