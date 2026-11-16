#include "iin_limit_command.h"
#include "device_commands.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Mirror of vin_limit_command.c (docs/charger-iin-limit.md). Its own busy flag:
 * a pending VIN request does not refuse an IIN one, and vice versa; the driver
 * serialises the two on the I2C bus mutex. */
static iin_limit_command_config_t s_config;
static atomic_bool s_busy;

typedef struct {
    char id[65];
    bool write;
    uint16_t ma;
} request_t;

static void report(const request_t *request, bool ok, const power_iin_limit_t *limit,
                   const char *detail)
{
    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) return;
    cJSON_AddStringToObject(reply, "type", request->write ? "set_iin_limit_result" : "iin_limit_result");
    cJSON_AddStringToObject(reply, "id", request->id);
    cJSON_AddBoolToObject(reply, "ok", ok);
    if (limit != NULL) {
        /* `ma` is the REGISTER limit. en_ilim=true means the ILIM-pin resistor
         * also limits and the lower of the two applies. */
        cJSON_AddNumberToObject(reply, "ma", limit->ma);
        cJSON_AddNumberToObject(reply, "reg00", limit->reg00);
        cJSON_AddBoolToObject(reply, "en_ilim", (limit->reg00 & POWER_IIN_EN_ILIM_BIT) != 0);
        cJSON_AddBoolToObject(reply, "hiz", (limit->reg00 & POWER_IIN_HIZ_BIT) != 0);
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
    power_iin_limit_t limit;
    cmd_result_t result = request->write ? cmd_set_iin_limit(request->ma, &limit)
                                         : cmd_get_iin_limit(&limit);
    report(request, result.status == ESP_OK, result.status == ESP_OK ? &limit : NULL,
           result.message);
    free(request);
    atomic_store(&s_busy, false);
}

void iin_limit_command_init(const iin_limit_command_config_t *config)
{
    s_config = *config;
}

bool iin_limit_command_handle(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) return false;
    const bool write = strcmp(type->valuestring, "set_iin_limit") == 0;
    if (!write && strcmp(type->valuestring, "get_iin_limit") != 0) return false;

    request_t request = {.write = write};
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) >= sizeof request.id) {
        report(&request, false, NULL, "invalid_id");
        return true;
    }
    memcpy(request.id, id->valuestring, strlen(id->valuestring) + 1);
    if (write) {
        const cJSON *ma = cJSON_GetObjectItemCaseSensitive(root, "ma");
        if (!cJSON_IsNumber(ma) || !isfinite(ma->valuedouble) ||
            ma->valuedouble < POWER_IIN_LIMIT_MIN_MA || ma->valuedouble > POWER_IIN_LIMIT_MAX_MA ||
            ma->valuedouble != (double)ma->valueint ||
            !power_iin_limit_valid_ma((uint32_t)ma->valueint)) {
            report(&request, false, NULL, "invalid_ma: expected 100..3250 in 50 mA steps");
            return true;
        }
        request.ma = (uint16_t)ma->valueint;
    }

    /* A failed/slow I2C bus can wait a second per transaction. Never perform
     * that work on esp-mqtt, where it would delay PUBACK and disconnect handling.
     * One pending IIN request bounds allocations and maintenance-queue use. */
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
