#include "telemetry_publish.h"
#include "envelope_provenance.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"

esp_err_t telemetry_publish(const telemetry_publish_config_t *cfg,
                            const char *sample_json, int64_t observed_ms,
                            uint16_t battery_mv)
{
    if (cfg == NULL || cfg->publish == NULL || cfg->topic == NULL ||
        cfg->topic[0] == '\0' || cfg->device_id == NULL || sample_json == NULL)
        return ESP_ERR_INVALID_ARG;

    esp_err_t result = ESP_ERR_NO_MEM;
    cJSON *envelope = cJSON_CreateObject();
    cJSON *provenance_json = NULL;
    char *wire = NULL;
    /* The drain task has its own static scratch. This task must never share it,
     * including while a schedule reload or backlog publish is in progress. */
    schedule_provenance_t *provenance = NULL;
    char *part = NULL;
    if (envelope == NULL) goto cleanup;
    cJSON *samples = cJSON_AddArrayToObject(envelope, "sample");
    if (samples == NULL) goto cleanup;
    /* Preserve the builder's integer IDs verbatim (cJSON numbers are doubles). */
    cJSON *sample = cJSON_CreateRaw(sample_json);
    if (sample == NULL) goto cleanup;
    if (!cJSON_AddItemToArray(samples, sample)) {
        cJSON_Delete(sample);
        goto cleanup;
    }
    time_t seconds = (time_t)(observed_ms / 1000);
    struct tm utc;
    char timestamp[32];
    if (gmtime_r(&seconds, &utc) == NULL ||
        strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        result = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (!cJSON_AddStringToObject(envelope, "timestamp", timestamp) ||
        !cJSON_AddStringToObject(envelope, "device_id", cfg->device_id) ||
        !cJSON_AddStringToObject(envelope, "device_name", cfg->device_name ? cfg->device_name : "") ||
        !cJSON_AddStringToObject(envelope, "device_version", cfg->device_version ? cfg->device_version : "") ||
        !cJSON_AddStringToObject(envelope, "device_firmware", cfg->device_firmware ? cfg->device_firmware : ""))
        goto cleanup;
    if (cfg->timezone && cfg->timezone[0] &&
        !cJSON_AddStringToObject(envelope, "timezone", cfg->timezone)) goto cleanup;
    if (battery_mv && !cJSON_AddNumberToObject(envelope, "device_battery", (double)battery_mv / 1000.0)) goto cleanup;

    if (cfg->provenance) {
        provenance = calloc(1, sizeof *provenance);
        if (provenance == NULL) goto cleanup;
        if (cfg->provenance(provenance) == ESP_OK) {
            /* Same 1440-byte bound and per-sample macro filtering as the drain.
             * Turn its trailing-comma splice into an object for safe merging. */
            part = malloc(1442);
            if (part == NULL) goto cleanup;
            part[0] = '{';
            int length = envelope_provenance_part(provenance, sample_json, part + 1, 1440);
            if (length > 0) {
                part[length] = '}';
                part[length + 1] = '\0';
                provenance_json = cJSON_Parse(part);
                if (provenance_json == NULL) goto cleanup;
                while (provenance_json->child) {
                    cJSON *field = cJSON_DetachItemViaPointer(provenance_json, provenance_json->child);
                    if (!cJSON_AddItemToObject(envelope, field->string, field)) {
                        cJSON_Delete(field);
                        goto cleanup;
                    }
                }
            }
        }
    }
    wire = cJSON_PrintUnformatted(envelope);
    if (wire == NULL) goto cleanup;
    result = cfg->publish(cfg->topic, wire, strlen(wire), NULL);
cleanup:
    cJSON_free(wire);
    cJSON_Delete(provenance_json);
    cJSON_Delete(envelope);
    free(part);
    free(provenance);
    return result;
}
