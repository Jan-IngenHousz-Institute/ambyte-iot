#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "payload_v3.h"
#include "telemetry_publish.h"

static int published, degraded;
static int fail_after = -1, allocations;
static bool transport_failure;
static const char *scenario;
static bool is(const char *s) { return strcmp(scenario, s) == 0; }
static void *allocate(size_t n)
{
    if (fail_after >= 0 && allocations++ == fail_after) return NULL;
    return malloc(n);
}
static esp_err_t provenance(schedule_provenance_t *out)
{
    if (is("no_schedule")) return ESP_ERR_NOT_FOUND;
    strcpy(out->workbook_version_id, "test-workbook-id");
    out->macro_count = 2;
    for (int i = 0; i < 2; ++i) {
        strcpy(out->macros[i].id, i ? "trace-id" : "telemetry-id");
        strcpy(out->macros[i].name, i ? "trace" : "telemetry");
        strcpy(out->macros[i].filename, i ? "trace.py" : "telemetry.py");
        out->macros[i].cond_count = 1;
        out->macros[i].conds[0].field = SCHED_PROV_COND_FIELD_SCHEMA;
        out->macros[i].conds[0].op = SCHED_PROV_COND_OP_EQ;
        strcpy(out->macros[i].conds[0].value, i ? "ambit.trace/3" : "ambyte.telemetry/1");
    }
    return ESP_OK;
}
static esp_err_t publish(const char *topic, const char *payload, size_t len, int *id)
{
    assert(strcmp(topic, "experiment/data_ingest/v1/experiment/multispeq/v1.0/ambyte_MAC") == 0);
    assert(len == strlen(payload));
    assert(id == NULL); /* Direct heartbeat never enrolls in the stored-event window. */
    ++published;
    if (is("allocations")) {
        bool has_workbook = strstr(payload, "workbook_version_id") != NULL;
        bool has_macros = strstr(payload, "telemetry-id") != NULL;
        assert(has_workbook == has_macros); /* Never attach half the routing pair. */
        if (!has_workbook) ++degraded;
    }
    if (!is("allocations")) puts(payload);
    /* The sample's int64 ID must not round through cJSON's double. */
    assert(strstr(payload, "9007199254740993"));
    return transport_failure ? ESP_FAIL : ESP_OK;
}
int main(int argc, char **argv)
{
    assert(argc == 2); scenario = argv[1];
    const int64_t now = is("clock_unset") ? 0 : 1789714800000LL;
    payload_v3_telemetry_input_t input = {
        .measure_id = 9007199254740993LL, .device = "MAC", .observed_utc_ms = now,
        .connectivity_valid = true, .wifi = true, .provisioned = true,
        .publish_gate = false, .power_valid = !is("power_error"),
        .battery_v = is("critical") ? 3.1 : 3.8,
        .input_present = is("charging"), .charge_status = is("charging") ? 2 : 0,
        .storage_db_valid = true, .db_online = false,
        .runtime_valid = true, .uptime_s = 900,
        .software_valid = true, .firmware = "2.2.3",
    };
    char sample[4096], error[96];
    assert(payload_v3_build_telemetry(sample, sizeof sample, &input, error, sizeof error));
    telemetry_publish_config_t cfg = {
        .publish = publish,
        .topic = "experiment/data_ingest/v1/experiment/multispeq/v1.0/ambyte_MAC",
        .device_id = "MAC", .device_name = "Ambyte \"test\"\n",
        .device_version = "V003", .device_firmware = "2.2.3",
        .timezone = "Europe/Amsterdam", .provenance = provenance,
    };
    transport_failure = is("publish_failure");
    if (is("optional")) {
        cfg.provenance = NULL; cfg.timezone = NULL;
        cfg.device_name = NULL; cfg.device_version = NULL; cfg.device_firmware = NULL;
    }
    assert(telemetry_publish(NULL, sample, now, 0) == ESP_ERR_INVALID_ARG);
    if (is("allocations")) {
        cJSON_Hooks hooks = {.malloc_fn = allocate, .free_fn = free};
        cJSON_InitHooks(&hooks);
        int index;
        for (index = 0; index < 300; ++index) {
            fail_after = index; allocations = 0; published = 0;
            esp_err_t result = telemetry_publish(&cfg, sample, now, 3800);
            if (result == ESP_OK) {
                assert(published == 1);
                if (allocations <= fail_after) break; /* Covered every allocation. */
            } else {
                assert(result == ESP_ERR_NO_MEM && published == 0);
            }
        }
        assert(index > 20 && index < 300);
        assert(degraded > 2); /* Snapshot, splice and parse failpoints publish without provenance. */
    } else {
        esp_err_t result = telemetry_publish(&cfg, sample, now,
            is("power_error") ? 0 : is("critical") ? 3100 : 3800);
        assert(result == (transport_failure ? ESP_FAIL : ESP_OK));
        assert(published == 1);
    }
    return 0;
}
