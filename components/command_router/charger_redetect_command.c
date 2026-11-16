#include "charger_redetect_command.h"
#include "device_commands.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* docs/charger-redetect.md. Same shape as vin_limit_command.c: validation and
 * one bounded enqueue on the esp-mqtt task, everything else on the worker. */

static charger_redetect_command_config_t s_config;
static atomic_bool s_busy;

typedef struct {
    char id[65];
    bool auto_switch;              /* charger_redetect_auto */
    int  set;                      /* auto: -1 query, 0 off, 1 on */
    power_redetect_method_t method;
} request_t;

static void emit(cJSON *reply)
{
    char *json = cJSON_PrintUnformatted(reply);
    if (json != NULL) {
        if (s_config.reply != NULL) s_config.reply(json);
        cJSON_free(json);
    }
    cJSON_Delete(reply);
}

static cJSON *begin(const request_t *request, bool ok, const char *detail)
{
    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) return NULL;
    cJSON_AddStringToObject(reply, "type", request->auto_switch ? "charger_redetect_auto_result"
                                                                 : "charger_redetect_result");
    cJSON_AddStringToObject(reply, "id", request->id);
    cJSON_AddBoolToObject(reply, "ok", ok);
    if (!request->auto_switch) {
        cJSON_AddStringToObject(reply, "method",
                                request->method == POWER_REDETECT_DPDM ? "dpdm" : "bg_en");
    }
    cJSON_AddStringToObject(reply, "detail", detail);
    return reply;
}

static void report_error(const request_t *request, const char *detail)
{
    cJSON *reply = begin(request, false, detail);
    if (reply != NULL) emit(reply);
}

/* Registers as one 48-character hex string ("--" = not read): four full
 * snapshots stay well under 1 KiB of reply. */
static void add_snapshot(cJSON *array, const power_charger_snapshot_t *s)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) return;
    char hex[POWER_CHARGER_REG_COUNT * 2 + 1];
    for (unsigned r = 0; r < POWER_CHARGER_REG_COUNT; r++) {
        if (s->mask & (1u << r)) snprintf(hex + 2 * r, 3, "%02X", s->regs[r]);
        else memcpy(hex + 2 * r, "--", 3);
    }
    cJSON_AddNumberToObject(o, "t_ms", s->t_ms);
    cJSON_AddNumberToObject(o, "mask", s->mask);
    cJSON_AddStringToObject(o, "regs", hex);
    if (s->mask & (1u << 0x0C)) {
        cJSON_AddNumberToObject(o, "vin_stat", (s->regs[0x0C] >> 5) & 7u);
        cJSON_AddNumberToObject(o, "chg_stat", (s->regs[0x0C] >> 3) & 3u);
    }
    if (s->mask & (1u << 0x14)) cJSON_AddBoolToObject(o, "vinppm", (s->regs[0x14] & 0x80u) != 0);
    cJSON_AddItemToArray(array, o);
}

static void execute(void *arg)
{
    request_t *request = arg;
    if (request->auto_switch) {
        power_redetect_auto_t st;
        memset(&st, 0, sizeof st);
        const cmd_result_t r = cmd_charger_redetect_auto(request->set, &st);
        cJSON *reply = begin(request, r.status == ESP_OK, r.message);
        if (reply != NULL && r.status == ESP_OK) {
            cJSON_AddBoolToObject(reply, "enabled", st.enabled);
            cJSON_AddStringToObject(reply, "state", st.state);
            cJSON_AddStringToObject(reply, "disabled_reason", st.disabled_reason);
            cJSON_AddNumberToObject(reply, "attempts", st.attempts);
            cJSON_AddNumberToObject(reply, "charging", st.charging);
            cJSON_AddStringToObject(reply, "last_outcome", st.last_outcome);
            cJSON_AddNumberToObject(reply, "next_in_s", st.next_in_s);
        }
        if (reply != NULL) emit(reply);
    } else {
        power_redetect_report_t rep;
        memset(&rep, 0, sizeof rep);
        const cmd_result_t r = cmd_charger_redetect(request->method, &rep);
        cJSON *reply = begin(request, r.status == ESP_OK, r.message);
        if (reply != NULL && rep.snapshots > 0) {
            /* Evidence is reported on failure too: a non-latching BG_EN is a
             * finding, not just an error. */
            cJSON_AddNumberToObject(reply, "reg", rep.reg);
            cJSON_AddNumberToObject(reply, "reg_before", rep.reg_before);
            cJSON_AddNumberToObject(reply, "reg_set", rep.reg_set);
            cJSON_AddNumberToObject(reply, "reg_after", rep.reg_after);
            cJSON_AddBoolToObject(reply, "latched", rep.latched);
            cJSON_AddBoolToObject(reply, "cleared", rep.cleared);
            cJSON_AddBoolToObject(reply, "restored", rep.restored);
            cJSON *snaps = cJSON_AddArrayToObject(reply, "snapshots");
            for (unsigned i = 0; snaps != NULL && i < rep.snapshots && i < POWER_REDETECT_SNAPSHOTS; i++) {
                add_snapshot(snaps, &rep.snap[i]);
            }
        }
        if (reply != NULL) emit(reply);
    }
    free(request);
    atomic_store(&s_busy, false);
}

void charger_redetect_command_init(const charger_redetect_command_config_t *config)
{
    s_config = *config;
}

bool charger_redetect_command_handle(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) return false;
    const bool auto_switch = strcmp(type->valuestring, "charger_redetect_auto") == 0;
    if (!auto_switch && strcmp(type->valuestring, "charger_redetect") != 0) return false;

    request_t request = {.auto_switch = auto_switch, .set = -1, .method = POWER_REDETECT_BG_EN};
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) >= sizeof request.id) {
        report_error(&request, "invalid_id");
        return true;
    }
    memcpy(request.id, id->valuestring, strlen(id->valuestring) + 1);
    if (auto_switch) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (enabled != NULL) {
            if (!cJSON_IsBool(enabled)) {
                report_error(&request, "invalid_enabled: expected true, false or omitted (query)");
                return true;
            }
            request.set = cJSON_IsTrue(enabled) ? 1 : 0;
        }
    } else {
        const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
        if (method != NULL) {
            if (cJSON_IsString(method) && strcmp(method->valuestring, "bg_en") == 0) {
                request.method = POWER_REDETECT_BG_EN;
            } else if (cJSON_IsString(method) && strcmp(method->valuestring, "dpdm") == 0) {
                request.method = POWER_REDETECT_DPDM;
            } else {
                report_error(&request, "invalid_method: expected bg_en or dpdm");
                return true;
            }
        }
    }

    /* A re-detect blocks ~5 s and touches the I2C bus: never on esp-mqtt,
     * where it would delay PUBACK and disconnect handling. One pending
     * request bounds allocations and maintenance-queue use. */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        report_error(&request, "busy");
        return true;
    }
    request_t *copy = malloc(sizeof *copy);
    if (copy != NULL) *copy = request;
    if (copy == NULL || s_config.submit == NULL || !s_config.submit(execute, copy)) {
        free(copy);
        atomic_store(&s_busy, false);
        report_error(&request, "queue_unavailable");
    }
    return true;
}
