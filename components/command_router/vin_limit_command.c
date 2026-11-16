#include "vin_limit_command.h"
#include "device_commands.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static vin_limit_command_config_t s_config;
static atomic_bool s_busy;

typedef struct {
    char id[65];
    bool write;
    uint16_t mv;
} request_t;

static void report(const request_t *request, bool ok, const power_vin_limit_t *limit,
                   const char *detail)
{
    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) return;
    cJSON_AddStringToObject(reply, "type", request->write ? "set_vin_limit_result" : "vin_limit_result");
    cJSON_AddStringToObject(reply, "id", request->id);
    cJSON_AddBoolToObject(reply, "ok", ok);
    if (limit != NULL) {
        cJSON_AddNumberToObject(reply, "mv", limit->mv);
        cJSON_AddNumberToObject(reply, "reg01", limit->reg01);
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
    power_vin_limit_t limit;
    cmd_result_t result = request->write ? cmd_set_vin_limit(request->mv, &limit)
                                         : cmd_get_vin_limit(&limit);
    report(request, result.status == ESP_OK, result.status == ESP_OK ? &limit : NULL,
           result.message);
    free(request);
    atomic_store(&s_busy, false);
}

void vin_limit_command_init(const vin_limit_command_config_t *config)
{
    s_config = *config;
}

bool vin_limit_command_handle(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) return false;
    const bool write = strcmp(type->valuestring, "set_vin_limit") == 0;
    if (!write && strcmp(type->valuestring, "get_vin_limit") != 0) return false;

    request_t request = {.write = write};
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) >= sizeof request.id) {
        report(&request, false, NULL, "invalid_id");
        return true;
    }
    memcpy(request.id, id->valuestring, strlen(id->valuestring) + 1);
    if (write) {
        const cJSON *mv = cJSON_GetObjectItemCaseSensitive(root, "mv");
        if (!cJSON_IsNumber(mv) || !isfinite(mv->valuedouble) ||
            mv->valuedouble < POWER_VIN_LIMIT_MIN_MV || mv->valuedouble > POWER_VIN_LIMIT_MAX_MV ||
            mv->valuedouble != (double)mv->valueint ||
            !power_vin_limit_valid_mv((uint32_t)mv->valueint)) {
            report(&request, false, NULL, "invalid_mv: expected 3700..15200 in 100 mV steps");
            return true;
        }
        request.mv = (uint16_t)mv->valueint;
    }

    /* A failed/slow I2C bus can wait a second per transaction. Never perform
     * that work on esp-mqtt, where it would delay PUBACK and disconnect handling.
     * One pending VIN request bounds allocations and maintenance-queue use. */
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
