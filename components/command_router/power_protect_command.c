/*
 * power_protect_command.c — MQTT power_protect (contract:
 * include/power_protect_command.h). Two task contexts, as log_command.c:
 *   esp-mqtt task : power_protect_command_handle() — parse, validate, copy,
 *                   enqueue. Never touches the power_protect mutex or NVS.
 *   maint worker  : execute() — status read, or maintenance lock → force /
 *                   clear → unlock, then publish.
 */
#include "power_protect_command.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static power_protect_command_config_t s_config;
/* One power_protect operation at a time: a QoS 1 duplicate gets "busy" at
 * once instead of queueing a second write behind the first. */
static atomic_bool s_busy;

typedef enum { ACT_STATUS = 0, ACT_CLEAR, ACT_FORCE } action_t;

typedef struct {
    char       id[65];
    action_t   action;
    pp_state_t state;   /* force target */
} request_t;

static const char *action_name(action_t a)
{
    return a == ACT_CLEAR ? "clear" : a == ACT_FORCE ? "force" : "status";
}

/* ── allocation-free refusals ──────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t cap, len;
    bool   ok;
} sbuf_t;

static void sb_raw(sbuf_t *b, const char *s)
{
    const size_t n = strlen(s);
    if (!b->ok || b->len + n >= b->cap) { b->ok = false; return; }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* JSON string, cJSON's escaping rules (quote, backslash, control bytes). */
static void sb_str(sbuf_t *b, const char *s)
{
    sb_raw(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p && b->ok; p++) {
        char e[8];
        if (*p == '"' || *p == '\\') { e[0] = '\\'; e[1] = (char)*p; e[2] = '\0'; }
        else if (*p < 0x20) snprintf(e, sizeof e, "\\u%04x", *p);
        else { e[0] = (char)*p; e[1] = '\0'; }
        sb_raw(b, e);
    }
    sb_raw(b, "\"");
}

/* {"type","id","ok":false,"detail",["action"],"state"}: id ≤ 64 B (≤ 384
 * escaped), every other string a fixed enum name. */
static void send_refusal(const char *id, const char *action, const char *detail)
{
    char buf[640];
    sbuf_t b = {.buf = buf, .cap = sizeof buf, .len = 0, .ok = true};
    buf[0] = '\0';
    sb_raw(&b, "{\"type\":\"power_protect_result\",\"id\":");
    sb_str(&b, id != NULL ? id : "");
    sb_raw(&b, ",\"ok\":false,\"detail\":");
    sb_str(&b, detail);
    if (action != NULL) {
        sb_raw(&b, ",\"action\":");
        sb_str(&b, action);
    }
    if (s_config.state_now != NULL) {
        sb_raw(&b, ",\"state\":");
        sb_str(&b, pp_state_name(s_config.state_now()));
    }
    sb_raw(&b, "}");
    if (b.ok && s_config.reply) s_config.reply(buf);
}

/* ── worker ────────────────────────────────────────────────────────────── */

/* Full reply, or NULL on any allocation failure (never a partial ok). */
static char *render(const request_t *r, bool ok, const char *detail,
                    const power_protect_status_t *st)
{
    cJSON *j = cJSON_CreateObject();
    if (j == NULL) return NULL;
    cJSON *sk = NULL;
    const bool built =
        cJSON_AddStringToObject(j, "type", "power_protect_result") &&
        cJSON_AddStringToObject(j, "id", r->id) &&
        cJSON_AddBoolToObject(j, "ok", ok) &&
        cJSON_AddStringToObject(j, "action", action_name(r->action)) &&
        cJSON_AddStringToObject(j, "detail", detail) &&
        cJSON_AddStringToObject(j, "state", pp_state_name(st->state)) &&
        cJSON_AddStringToObject(j, "reason", pp_reason_name(st->reason)) &&
        cJSON_AddBoolToObject(j, "forced", st->forced) &&
        (st->since_utc != 0 ? cJSON_AddNumberToObject(j, "since_utc", (double)st->since_utc) != NULL
                            : cJSON_AddNullToObject(j, "since_utc") != NULL) &&
        cJSON_AddNumberToObject(j, "brownouts_6h", st->brownouts_6h) &&
        (st->exit_in_s >= 0 ? cJSON_AddNumberToObject(j, "exit_in_s", (double)st->exit_in_s) != NULL
                            : cJSON_AddNullToObject(j, "exit_in_s") != NULL) &&
        (sk = cJSON_AddObjectToObject(j, "skipped")) != NULL &&
        cJSON_AddNumberToObject(sk, "flash_runs", st->skipped.flash_runs) &&
        cJSON_AddNumberToObject(sk, "ambit_runs", st->skipped.ambit_runs) &&
        cJSON_AddNumberToObject(sk, "ota", st->skipped.ota);
    char *out = built ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    return out;
}

static void execute(void *arg)
{
    request_t *r = arg;
    bool ok = true;
    const char *detail = "ok";
    const char *refusal = NULL;
    if (r->action != ACT_STATUS) {
        if (!s_config.maintenance_begin || !s_config.maintenance_end || !s_config.maintenance_begin()) {
            refusal = "maintenance_active";
        } else {
            const esp_err_t err = r->action == ACT_FORCE
                ? (s_config.force ? s_config.force(r->state) : ESP_ERR_NOT_SUPPORTED)
                : (s_config.clear ? s_config.clear() : ESP_ERR_NOT_SUPPORTED);
            s_config.maintenance_end();
            ok = err == ESP_OK;
            detail = ok ? (r->action == ACT_FORCE ? "applied" : "cleared") : "persist_failed";
        }
    }
    if (refusal != NULL) {
        send_refusal(r->id, action_name(r->action), refusal);
    } else {
        power_protect_status_t st;
        memset(&st, 0, sizeof st);
        st.exit_in_s = -1;
        if (s_config.get_status) s_config.get_status(&st);
        char *json = render(r, ok, detail, &st);
        if (json != NULL) {
            if (s_config.reply) s_config.reply(json);
            cJSON_free(json);
        } else {
            send_refusal(r->id, action_name(r->action), "no_mem");
        }
    }
    cJSON_free(r);
    atomic_store(&s_busy, false);
}

/* ── esp-mqtt task: parse, validate, enqueue ───────────────────────────── */

void power_protect_command_init(const power_protect_command_config_t *config)
{
    s_config = *config;
}

bool power_protect_command_handle(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "power_protect") != 0) return false;

    request_t r;
    memset(&r, 0, sizeof r);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) >= sizeof r.id) {
        send_refusal("", NULL, "invalid_id");
        return true;
    }
    memcpy(r.id, id->valuestring, strlen(id->valuestring) + 1);

    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action)) {
        send_refusal(r.id, NULL, "invalid_action");
        return true;
    }
    if (strcmp(action->valuestring, "status") == 0)      r.action = ACT_STATUS;
    else if (strcmp(action->valuestring, "clear") == 0)  r.action = ACT_CLEAR;
    else if (strcmp(action->valuestring, "force") == 0)  r.action = ACT_FORCE;
    else {
        send_refusal(r.id, NULL, "invalid_action");
        return true;
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (r.action == ACT_FORCE) {
        if (!cJSON_IsString(state) || !pp_force_state_parse(state->valuestring, &r.state)) {
            send_refusal(r.id, action_name(r.action), "invalid_state");
            return true;
        }
    } else if (state != NULL && !cJSON_IsNull(state)) {
        /* A state on status/clear means the sender meant something else
         * (a force?): refuse rather than silently do the other thing. */
        send_refusal(r.id, action_name(r.action), "invalid_state");
        return true;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        send_refusal(r.id, action_name(r.action), "busy");
        return true;
    }
    request_t *copy = cJSON_malloc(sizeof *copy);
    if (copy) *copy = r;
    if (!copy || !s_config.submit || !s_config.submit(execute, copy)) {
        cJSON_free(copy);
        atomic_store(&s_busy, false);
        send_refusal(r.id, action_name(r.action), "queue_unavailable");
    }
    return true;
}
