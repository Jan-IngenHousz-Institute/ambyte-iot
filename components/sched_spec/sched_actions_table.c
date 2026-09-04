/*
 * sched_actions_table.c — the action catalog: declarations only.
 *
 * The C table IS the schema (design decision 3): each action declares its
 * inputs with type/required/range/default, the compiler validates `with:`
 * against it, and sched_actions_dump_json() emits the same table as JSON
 * Schema draft-07 fragments so CI can pin schedule/actions.schema.json to
 * the firmware. Run functions are NULL here; the runner (T3) binds them.
 */

#include "sched_spec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Ranges and defaults, with their whys: */
#define TRACE_MARGIN_MIN_MS 0
#define TRACE_MARGIN_MAX_MS 300000 /* 5 min: a margin beyond any sane grid is a typo */
#define TRACE_MARGIN_DEF_MS 15000  /* main.lua's "broken past est + 15 s" */
#define ACTINIC_LEVEL_MIN   (-32768) /* raw int16 on the wire */
#define ACTINIC_LEVEL_MAX   32767
#define ACTINIC_DUR_MAX_MS  600000 /* 10 min ≈ longest MPF relaxation; the light must always turn off */
#define SLEEP_MAX_MS        60000  /* design catalog: device/sleep ≤ 60 s */

/* channels is OPTIONAL on the four ambit actions: the same document is
 * shared by every device in an experiment, and an explicit channel list is
 * device topology, not experiment intent. has_default materializes an
 * absent channels as n == 0 = "all channels that answer a ping" (see
 * fill_entry); an explicit non-empty list stays valid and means a
 * deliberate restriction. */
static const sched_input_decl_t k_trace_inputs[] = {
    { "protocol",        SCHED_IN_STRING,      1, 0, 0, 0, 0, NULL },
    { "channels",        SCHED_IN_CHANNELS,    0, 1, 0, 0, 0, NULL },
    { "hold_window",     SCHED_IN_BOOL,        0, 1, 0, 0, 0, NULL },
    { "tag",             SCHED_IN_STRING,      0, 0, 0, 0, 0, NULL }, /* default: protocol name (runtime) */
    { "deadline_margin", SCHED_IN_DURATION_MS, 0, 1,
      TRACE_MARGIN_MIN_MS, TRACE_MARGIN_MAX_MS, TRACE_MARGIN_DEF_MS, NULL },
};

static const sched_input_decl_t k_channels_only[] = {
    { "channels", SCHED_IN_CHANNELS, 0, 1, 0, 0, 0, NULL },
};

static const sched_input_decl_t k_actinic_inputs[] = {
    { "channels", SCHED_IN_CHANNELS,    0, 1, 0, 0, 0, NULL },
    { "level",    SCHED_IN_INT,         1, 0, ACTINIC_LEVEL_MIN, ACTINIC_LEVEL_MAX, 0, NULL },
    /* duration is mandatory so a job cannot leave the light on */
    { "duration", SCHED_IN_DURATION_MS, 1, 0, 1, ACTINIC_DUR_MAX_MS, 0, NULL },
};

static const sched_input_decl_t k_status_report_inputs[] = {
    { "tags", SCHED_IN_MAP, 0, 0, 0, 0, 0, NULL },
};

static const sched_input_decl_t k_store_event_inputs[] = {
    { "channel",  SCHED_IN_INT,    0, 0, 0, SCHED_SPEC_MAX_CHANNELS - 1, 0, NULL },
    { "data",     SCHED_IN_MAP,    0, 0, 0, 0, 0, NULL },
    { "metadata", SCHED_IN_MAP,    0, 0, 0, 0, 0, NULL },
    /* required: every custom event carries data.kind (catalog wording), and
     * the runner stamps it unconditionally — an omitted kind would dereference
     * NULL on device (T3 review blocker 5) */
    { "kind",     SCHED_IN_STRING, 1, 0, 0, 0, 0, NULL },
};

static const sched_input_decl_t k_log_inputs[] = {
    { "message", SCHED_IN_STRING, 1, 0, 0, 0, 0, NULL },
};

static const sched_input_decl_t k_sleep_inputs[] = {
    { "duration", SCHED_IN_DURATION_MS, 1, 0, 1, SLEEP_MAX_MS, 0, NULL },
};

#define ACTION(n, arr) { n, arr, (uint8_t)(sizeof(arr) / sizeof((arr)[0])), NULL, NULL }

/* NOT const: the runner binds run/run_ctx into the rows at start
 * (sched_action_bind). Everything the compiler and the JSON dump read stays
 * immutable after that. */
static sched_action_t k_actions[] = {
    ACTION("ambit/trace",          k_trace_inputs),
    ACTION("ambit/spectrum",       k_channels_only),
    ACTION("ambit/leaf-temp",      k_channels_only),
    ACTION("ambit/actinic",        k_actinic_inputs),
    ACTION("device/status-report", k_status_report_inputs),
    ACTION("db/store-event",       k_store_event_inputs),
    ACTION("device/log",           k_log_inputs),
    ACTION("device/sleep",         k_sleep_inputs),
};

const sched_action_t *sched_actions_table(size_t *count)
{
    if (count != NULL) *count = sizeof(k_actions) / sizeof(k_actions[0]);
    return k_actions;
}

const sched_action_t *sched_action_find(const char *name)
{
    if (name == NULL) return NULL;
    for (size_t i = 0; i < sizeof(k_actions) / sizeof(k_actions[0]); i++) {
        if (strcmp(k_actions[i].name, name) == 0) return &k_actions[i];
    }
    return NULL;
}

esp_err_t sched_action_bind(const char *name, void *run_ctx,
                            esp_err_t (*run)(void *ctx,
                                             const struct sched_step *step,
                                             const struct sched_program *prog))
{
    if (name == NULL || run == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < sizeof(k_actions) / sizeof(k_actions[0]); i++) {
        if (strcmp(k_actions[i].name, name) == 0) {
            k_actions[i].run     = run;
            k_actions[i].run_ctx = run_ctx;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ── db/store-event placeholders (design catalog row) ────────────────── */

static const char *const k_placeholders[] = {
    "$deployment", "$lat", "$lon", "$tz", "$boot_epoch", "$uptime_ms",
    "$sd_ready", "$job.runs", "$job.failures", "$job.skipped",
    "$job.skipped_saturated", "$job.fail_streak",
};

bool sched_is_placeholder(const char *s)
{
    if (s == NULL || s[0] != '$') return false;
    for (size_t i = 0; i < sizeof(k_placeholders) / sizeof(k_placeholders[0]); i++) {
        if (strcmp(k_placeholders[i], s) == 0) return true;
    }
    return false;
}

/* ── JSON Schema dump ────────────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t cap, len;
} jw_t;

static void jw(jw_t *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    /* NULL/0 probe-safe: vsnprintf(NULL, 0) just counts (C99), so a caller
     * can size the buffer with sched_actions_dump_json(NULL, 0) without any
     * pointer arithmetic on NULL here */
    char  *dst  = (w->buf != NULL && w->len < w->cap) ? w->buf + w->len : NULL;
    size_t room = dst != NULL ? w->cap - w->len : 0;
    int n = vsnprintf(dst, room, fmt, ap);
    va_end(ap);
    if (n > 0) w->len += (size_t)n;
}

static const char *json_type(uint8_t type)
{
    switch (type) {
    case SCHED_IN_INT:     return "{\"type\":\"integer\"";
    case SCHED_IN_FLOAT:   return "{\"type\":\"number\"";
    case SCHED_IN_BOOL:    return "{\"type\":\"boolean\"";
    case SCHED_IN_STRING:  return "{\"type\":\"string\"";
    default:               return "{\"type\":\"string\"";
    }
}

size_t sched_actions_dump_json(char *buf, size_t cap)
{
    jw_t w = { buf, cap, 0 };
    jw(&w, "{\"$schema\":\"http://json-schema.org/draft-07#\",");
    jw(&w, "\"title\":\"ambyte-schedule-actions\",");
    jw(&w, "\"description\":\"generated from the firmware action table "
           "(components/sched_spec/sched_actions_table.c); do not hand-edit\",");
    jw(&w, "\"oneOf\":[");
    size_t count;
    const sched_action_t *actions = sched_actions_table(&count);
    for (size_t a = 0; a < count; a++) {
        const sched_action_t *act = &actions[a];
        jw(&w, "%s{\"type\":\"object\",\"properties\":{", a == 0 ? "" : ",");
        jw(&w, "\"uses\":{\"const\":\"%s\"},", act->name);
        jw(&w, "\"with\":{\"type\":\"object\",\"properties\":{");
        for (uint8_t i = 0; i < act->input_count; i++) {
            const sched_input_decl_t *in = &act->inputs[i];
            jw(&w, "%s\"%s\":", i == 0 ? "" : ",", in->name);
            switch (in->type) {
            case SCHED_IN_CHANNELS:
                jw(&w, "{\"type\":\"array\",\"items\":{\"type\":\"integer\","
                       "\"minimum\":0,\"maximum\":%d},\"uniqueItems\":true,"
                       "\"minItems\":1}", SCHED_SPEC_MAX_CHANNELS - 1);
                break;
            case SCHED_IN_DURATION_MS:
                jw(&w, "{\"type\":\"string\",\"pattern\":\"^[0-9]+(ms|s|m|h)$\"");
                if (in->min > 0 || in->max > 0) {
                    jw(&w, ",\"x-minMs\":%lld,\"x-maxMs\":%lld",
                       (long long)in->min, (long long)in->max);
                }
                if (in->has_default) jw(&w, ",\"defaultMs\":%lld", (long long)in->def_i);
                jw(&w, "}");
                break;
            case SCHED_IN_MAP:
                jw(&w, "{\"type\":\"object\",\"maxProperties\":%d,"
                       "\"additionalProperties\":{\"type\":[\"string\",\"number\",\"boolean\"]}}",
                       SCHED_SPEC_MAX_EVENT_KEYS);
                break;
            default:
                jw(&w, "%s", json_type(in->type));
                if (in->type == SCHED_IN_INT && (in->min != 0 || in->max != 0)) {
                    jw(&w, ",\"minimum\":%lld,\"maximum\":%lld",
                       (long long)in->min, (long long)in->max);
                }
                if (in->has_default && in->type == SCHED_IN_BOOL) {
                    jw(&w, ",\"default\":%s", in->def_i ? "true" : "false");
                }
                jw(&w, "}");
                break;
            }
        }
        jw(&w, "},\"required\":[");
        bool first = true;
        bool any_required = false;
        for (uint8_t i = 0; i < act->input_count; i++) {
            if (!act->inputs[i].required) continue;
            jw(&w, "%s\"%s\"", first ? "" : ",", act->inputs[i].name);
            first = false;
            any_required = true;
        }
        /* when any input is required, the action object must require `with`
         * itself — otherwise { "uses": "ambit/trace" } would pass the schema
         * while the compiler rejects it (the table is source of truth) */
        jw(&w, "],\"additionalProperties\":false}},\"required\":[\"uses\"%s]}",
           any_required ? ",\"with\"" : "");
    }
    jw(&w, "]}");
    return w.len;
}
