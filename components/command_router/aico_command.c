#include "aico_command.h"
#include "device_commands.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* get_aico / set_aico (docs/charger-aico.md). Same split as iin_limit_command.c:
 * validation and a bounded enqueue on the MQTT task, I2C on the worker, one
 * request in flight (its own busy flag). No MQTT `reset`: the hourly re-arm is
 * automatic, and the manual one is a USB-console test tool. */
static aico_command_config_t s_config;
static atomic_bool s_busy;

typedef struct {
    char id[65];
    bool write;
    bool enabled;
} request_t;

static void report(const request_t *request, bool ok, const power_aico_t *a, const char *detail)
{
    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) return;
    cJSON_AddStringToObject(reply, "type", request->write ? "set_aico_result" : "aico_result");
    cJSON_AddStringToObject(reply, "id", request->id);
    cJSON_AddBoolToObject(reply, "ok", ok);
    if (a != NULL) {
        cJSON_AddBoolToObject(reply, "enabled", a->enabled);
        cJSON_AddNumberToObject(reply, "reg02", a->reg02);
        /* REG00 register limit vs the limit AICO applies now (REG14). */
        cJSON_AddNumberToObject(reply, "iin_limit_ma", a->iin_limit_ma);
        cJSON_AddNumberToObject(reply, "iin_dpm_ma", a->iin_dpm_ma);
        cJSON_AddBoolToObject(reply, "aico_stat", a->aico_stat);
        cJSON_AddNumberToObject(reply, "resets", a->resets);
        cJSON_AddNumberToObject(reply, "failed", a->failed);
        if (a->has_reset) cJSON_AddNumberToObject(reply, "last_reset_ms", (double)a->last_reset_ms);
        else cJSON_AddNullToObject(reply, "last_reset_ms");
        cJSON_AddNumberToObject(reply, "next_check_in_s", a->next_check_in_s);
    }
    cJSON_AddStringToObject(reply, "detail", detail);
    char *json = cJSON_PrintUnformatted(reply);
    if (json != NULL) {
        if (s_config.reply != NULL) s_config.reply(json);
        cJSON_free(json);
    }
    cJSON_Delete(reply);
}

static void execute(void *arg)
{
    request_t *request = arg;
    power_aico_t aico;
    cmd_result_t result = request->write ? cmd_set_aico(request->enabled, &aico) : cmd_get_aico(&aico);
    report(request, result.status == ESP_OK, result.status == ESP_OK ? &aico : NULL, result.message);
    free(request);
    atomic_store(&s_busy, false);
}

void aico_command_init(const aico_command_config_t *config)
{
    s_config = *config;
}

bool aico_command_handle(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) return false;
    const bool write = strcmp(type->valuestring, "set_aico") == 0;
    if (!write && strcmp(type->valuestring, "get_aico") != 0) return false;

    request_t request = {.write = write};
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) >= sizeof request.id) {
        report(&request, false, NULL, "invalid_id");
        return true;
    }
    memcpy(request.id, id->valuestring, strlen(id->valuestring) + 1);
    if (write) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (!cJSON_IsBool(enabled)) {
            report(&request, false, NULL, "invalid_enabled: expected true or false");
            return true;
        }
        request.enabled = cJSON_IsTrue(enabled);
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        report(&request, false, NULL, "busy");
        return true;
    }
    request_t *copy = malloc(sizeof *copy);
    if (copy != NULL) *copy = request;
    if (copy == NULL || s_config.submit == NULL || !s_config.submit(execute, copy)) {
        free(copy);
        atomic_store(&s_busy, false);
        report(&request, false, NULL, "queue_unavailable");
    }
    return true;
}
