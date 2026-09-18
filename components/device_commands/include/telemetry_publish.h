#pragma once

#include "messaging_port.h"
#include "schedule_provenance_port.h"

typedef struct {
    message_publish_fn publish;
    const char *topic;
    const char *device_id;
    const char *device_name;
    const char *device_version;
    const char *device_firmware;
    const char *timezone;
    schedule_provenance_fn provenance;
} telemetry_publish_config_t;

/* Publish a firmware-built canonical sample through the normal ingest envelope.
 * Owns only temporary buffers: no event-log claims, cursor or ACK-window slots.
 * The caller allocates a unique measure_id before constructing the sample. */
esp_err_t telemetry_publish(const telemetry_publish_config_t *config,
                            const char *sample_json, int64_t observed_ms,
                            uint16_t battery_mv);
