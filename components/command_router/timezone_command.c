#include "timezone_command.h"
#include "device_config.h"
#include "nvs.h"
#include "timezone.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static timezone_command_config_t s_config;
static atomic_bool s_busy;

typedef struct {
    char id[65];
    char zone[48];
    bool write;
} request_t;

static void report(const request_t *request, bool ok, bool saved, const char *detail)
{
    /* Read back both facts. A persisted value alone is never reported as an
     * active timezone: a failed write/readback leaves the runtime untouched. */
    char stored[48] = "";
    (void)device_config_get_timezone(stored, sizeof stored);
    const char *active = timezone_current_name();
    cJSON *reply = cJSON_CreateObject();
    if (!reply) return;
    cJSON_AddStringToObject(reply, "type", request->write ? "set_timezone_result" : "timezone_result");
    cJSON_AddStringToObject(reply, "id", request->id);
    cJSON_AddBoolToObject(reply, "ok", ok);
    cJSON_AddBoolToObject(reply, "saved", saved);
    cJSON_AddStringToObject(reply, "timezone", stored);
    cJSON_AddStringToObject(reply, "active_timezone", active);
    cJSON_AddBoolToObject(reply, "applied", ok && stored[0] && strcmp(stored, active) == 0);
    cJSON_AddNumberToObject(reply, "utc_offset_seconds", timezone_utc_offset_seconds((int64_t)time(NULL)));
    cJSON_AddStringToObject(reply, "detail", detail);
    char *json = cJSON_PrintUnformatted(reply);
    if (json) {
        if (s_config.reply) s_config.reply(json);
        cJSON_free(json);
    }
    cJSON_Delete(reply);
}

static void execute(void *arg)
{
    request_t *request = arg;
    if (!request->write) {
        report(request, true, false, "read_only");
        goto done;
    }
    if (!s_config.maintenance_begin || !s_config.maintenance_end || !s_config.maintenance_begin()) {
        report(request, false, false, "busy");
        goto done;
    }
    char stored[48] = "";
    esp_err_t err = device_config_get_timezone(stored, sizeof stored);
    /* Desired-state idempotency: QoS 1 duplicates and an explicit retry need
     * neither another NVS commit nor an RTC adjustment. Unknown reads refuse
     * rather than guessing whether a previous write persisted. */
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        report(request, false, false, "read_failed");
    } else {
        bool saved = err == ESP_OK && strcmp(stored, request->zone) == 0;
        if (!saved) saved = device_config_set_timezone(request->zone) == ESP_OK;
        if (!saved) {
            report(request, false, false, "persist_failed");
        } else if (device_config_get_timezone(stored, sizeof stored) != ESP_OK || strcmp(stored, request->zone) != 0) {
            report(request, false, false, "readback_failed");
        } else {
            /* Validated against the compiled IANA table before dispatch and
             * read back after commit. apply serializes libc TZ with readers;
             * publisher metadata reads the same immutable active-zone pointer.
             * No reboot, schedule replacement or UTC clock write is involved. */
            if (strcmp(timezone_current_name(), stored) != 0) timezone_apply(stored);
            bool applied = strcmp(timezone_current_name(), stored) == 0;
            report(request, applied, true, applied ? "applied" : "apply_failed");
        }
    }
    s_config.maintenance_end();
done:
    free(request);
    atomic_store(&s_busy, false);
}

void timezone_command_init(const timezone_command_config_t *config)
{
    s_config = *config;
}

bool timezone_command_handle(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) return false;
    bool write = strcmp(type->valuestring, "set_timezone") == 0;
    if (!write && strcmp(type->valuestring, "get_timezone") != 0) return false;
    request_t request = {.write = write};
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) >= sizeof request.id) {
        report(&request, false, false, "invalid_id");
        return true;
    }
    memcpy(request.id, id->valuestring, strlen(id->valuestring) + 1);
    const cJSON *zone = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    if (write && (!cJSON_IsString(zone) || timezone_canonicalize(zone->valuestring, request.zone, sizeof request.zone) != ESP_OK || !request.zone[0])) {
        report(&request, false, false, "invalid_timezone");
        return true;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        report(&request, false, false, "busy");
        return true;
    }
    request_t *copy = malloc(sizeof *copy);
    if (copy) *copy = request;
    if (!copy || !s_config.submit || !s_config.submit(execute, copy)) {
        free(copy);
        atomic_store(&s_busy, false);
        report(&request, false, false, "queue_unavailable");
    }
    return true;
}
