#ifndef AMBYTE_PAYLOAD_V3_H
#define AMBYTE_PAYLOAD_V3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAYLOAD_V3_MAX_SEGMENTS 16U
#define PAYLOAD_V3_MAX_ARRAYS   12U
#define PAYLOAD_V3_MAX_ATTACHED  4U
#define PAYLOAD_V3_TICK_FACTOR_MAX 100.0
/* AMBIT cmd 35 returns a fixed ten-bin spectrum plus a PAR scalar. */
#define PAYLOAD_V3_SPECTRUM_BINS 10U
/* Every producer MUST size its output buffer with this, not a hand-guessed
 * literal: a 320 B guess overflowed on the bench with a real 20-char ambit_name
 * and full-width bin counts, and the action correctly failed the store rather
 * than publishing a truncated object. Worst case ≈ 416 B — fixed keys ~210,
 * escaped device name ≤ 40, sensor_id 32, two epoch-ms stamps ~30, ten bins at
 * "65535," = 60 — so 512 leaves headroom without another heap tenant. */
#define PAYLOAD_V3_SPECTRUM_CAP 512U

typedef struct {
    uint8_t type;
    uint16_t pulses;
    uint16_t freq;
    uint8_t actinic;
    uint8_t subsampling;
} payload_v3_segment_t;

typedef struct {
    uint8_t index;
    uint16_t length;
    const uint32_t *values;
} payload_v3_array_t;

typedef struct {
    int64_t measure_id;
    const char *channel;
    const char *device;
    const char *sensor_id;
    int64_t start_utc_ms;
    int64_t end_utc_ms;
    const char *protocol_name;
    const char *protocol_id;
    /* Schedule-supplied label for WHY this run fired (e.g. "edge"). Orthogonal
     * to protocol_name — it is emitted as its own member so a consumer can still
     * filter on the protocol. Optional; NULL/"" omits it. */
    const char *protocol_tag;
    const char *protocol_cmd;
    const payload_v3_segment_t *segments;
    size_t segment_count;
    bool calibration_present;
    uint32_t cal_version;
    double tick_factor;
    bool gains_present;
    uint8_t gains[6];
    bool currents_present;
    uint8_t currents[3];
    const payload_v3_array_t *arrays;
    size_t array_count;
} payload_v3_trace_input_t;

typedef struct {
    bool present;
    const char *channel;
    const char *sensor_id;
    const char *firmware;
    uint8_t hardware_revision;
    const char *name;
    bool cal_version_present;
    uint32_t cal_version;
} payload_v3_attached_sensor_t;

typedef struct {
    int64_t measure_id;
    const char *device;
    int64_t observed_utc_ms;

    bool observations_valid;
    double air_temperature;
    double relative_humidity;
    double air_pressure;

    bool connectivity_valid;
    bool wifi;
    bool provisioned;
    bool publish_gate;
    uint32_t mqtt_reconnects;
    const char *last_disc_reason;
    int64_t conn_age_s;
    int64_t pending;

    bool power_valid;
    double battery_v;
    double input_v;
    double system_v;
    uint32_t input_ma;
    uint32_t charge_ma;
    bool input_present;
    uint8_t charge_status;

    bool storage_db_valid;
    bool db_online;
    bool storage_sd_valid;
    uint64_t sd_free_kb;
    int64_t sd_skipped;
    int64_t sd_dropped;
    int64_t last_acked_id;
    bool sd_io_lost;

    bool runtime_valid;
    int64_t uptime_s;
    uint32_t psram_free_kb;
    uint32_t psram_largest_kb;
    uint32_t psram_size_kb;
    uint32_t heap_dma_largest_kb;
    uint32_t heap_int_free_kb;
    uint32_t heap_int_largest_kb;
    bool wd_armed;
    const char *last_wd_reboot_reason;

    bool clock_valid;
    const char *clock_source;
    bool clock_suspect;

    bool software_valid;
    const char *firmware;
    bool script_valid;
    const char *script_sha256;
    const char *script_version;
    const char *script_built_against_fw;
    const char *script_installed_on_fw;
    bool script_metadata_verified;

    payload_v3_attached_sensor_t attached[PAYLOAD_V3_MAX_ATTACHED];
    size_t attached_count;
} payload_v3_telemetry_input_t;

typedef struct {
    int64_t measure_id;
    const char *channel;
    const char *device;
    int64_t observed_utc_ms;
    const char *sensor_id;
    const char *name;
    const char *firmware;
    uint8_t hardware_revision;
    uint32_t cal_version;
    int32_t mlx_coef[14];
    uint32_t adpd[6];
    double temp_offset;
    double temp_slope;
    double actinic_coef;
    double spec_coef;
    uint16_t act[5];
    double mlx_emissivity;
    double sun_coef;
    double tick_factor;
} payload_v3_device_input_t;

/* Point read from AMBIT cmd 35 (`get_par`). Small enough to carry its samples
 * by value, unlike a trace's borrowed arrays. */
typedef struct {
    int64_t measure_id;
    const char *channel;
    const char *device;
    const char *sensor_id;
    int64_t start_utc_ms;
    int64_t end_utc_ms;
    bool calibration_present;
    uint32_t cal_version;
    double par;
    uint16_t spectrum[PAYLOAD_V3_SPECTRUM_BINS];
} payload_v3_spectrum_input_t;

typedef enum {
    PAYLOAD_TRACE_ROUTE_ERROR = 0,
    PAYLOAD_TRACE_ROUTE_V3,
    PAYLOAD_TRACE_ROUTE_V2,
} payload_trace_route_t;

/* Production JSON builders. They never allocate, always NUL-terminate when cap
 * is nonzero, and fail rather than emitting a partial or mixed-schema object.
 * error receives a stable diagnostic when supplied. */
bool payload_v3_build_trace(char *out, size_t cap,
                            const payload_v3_trace_input_t *input,
                            char *error, size_t error_cap);
/* Lossless measurement router: canonical v3 is preferred, while any
 * representability/precondition failure falls back to the unchanged v2 data
 * shape. metadata is only populated for the v2 route. */
payload_trace_route_t payload_v3_build_trace_lossless(
    char *out, size_t cap, char *metadata, size_t metadata_cap,
    const payload_v3_trace_input_t *input, char *error, size_t error_cap);
bool payload_v3_build_spectrum(char *out, size_t cap,
                               const payload_v3_spectrum_input_t *input,
                               char *error, size_t error_cap);
bool payload_v3_build_telemetry(char *out, size_t cap,
                                const payload_v3_telemetry_input_t *input,
                                char *error, size_t error_cap);
bool payload_v3_build_device(char *out, size_t cap,
                             const payload_v3_device_input_t *input,
                             char *error, size_t error_cap);

/* Schema routing is intentionally strict: only the three firmware-owned v3
 * families bypass the legacy v2 sample builder. */
bool payload_v3_is_canonical_object(const char *json);

bool payload_v3_can_fetch_retained(bool trigger_valid, size_t segment_count);

#ifdef __cplusplus
}
#endif

#endif
