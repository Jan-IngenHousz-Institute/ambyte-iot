#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENSOR_PORTS 4u
#define SENSOR_CAL_BYTES 140u
#define SENSOR_SPEC_BYTES 132u
#define SENSOR_SNAPSHOT_MAX 16384u
#define SENSOR_REFRESH_MS 60000u
#define SENSOR_SPEC_RETRY_MS 600000u
#define SENSOR_BLOCK_MS 900u

/* This is the existing measurement-facing cache view. Snapshot observation
 * clocks and full blocks are deliberately outside measurement provenance. */
typedef struct {
    bool valid;
    char device_id[18];
    char fw_version[16];
    uint8_t hw_rev;
    uint32_t cal_version;
    char ambit_name[20];
    float actinic_coef, tick_factor;
    bool gains_set;
    uint8_t gains[6];
    bool currents_set;
    uint8_t currents[3];
} ambit_device_info_t;

typedef struct {
    int64_t uptime_ms, utc_ms;
    uint8_t clock; /* 0 unavailable, 1 rtc, 2 hwm, 3 sntp */
    bool suspect, valid;
} sensor_stamp_t;

typedef struct {
    ambit_device_info_t identity;
    uint32_t generation;
    bool seen, present, have_cal, have_spec, revalidation_failed;
    sensor_stamp_t presence_stamp, fw_stamp, cal_stamp, spec_stamp;
    unsigned char calibration[SENSOR_CAL_BYTES], spectral[SENSOR_SPEC_BYTES];
} sensor_port_snapshot_t;

typedef struct {
    uint32_t wire_transactions[SENSOR_PORTS], deferred_acquisition;
    uint32_t run_start_waits, run_start_wait_max_us;
    uint32_t stdout_hold_max_us, pin_heap_bytes, pin_heap_peak_bytes;
    uint32_t job_start_waits, job_start_wait_max_us;
} sensor_metrics_t;

typedef struct {
    uint64_t boot_id;
    int64_t captured_uptime_ms;
    uint8_t busy;
    sensor_metrics_t refresh;
    sensor_port_snapshot_t ports[SENSOR_PORTS];
} sensor_snapshot_t;

typedef enum {
    SENSOR_READ_OK, SENSOR_READ_ABSENT, SENSOR_READ_UNSUPPORTED,
    SENSOR_READ_FAILED
} sensor_read_result_t;
typedef sensor_read_result_t (*sensor_inspect_fn_t)(uint8_t ch, uint8_t type, void *out,
    size_t size, size_t *got, uint32_t timeout_ms, void (*started)(uint8_t));

/* inspect(type 0 = wake/presence, 1/2/4 = cmd33 subtype) uses one absolute
 * deadline including mutex acquisition. It invokes started immediately before
 * its first wake byte, never for cached/refused work. Callback ownership is
 * synchronous. No production algorithms are replaced by host implementations. */
typedef struct {
    int64_t (*now_us)(void);
    int64_t (*utc_ms)(void);
    uint8_t (*clock)(bool *suspect);
    void (*lock)(void), (*unlock)(void), (*yield_ms)(uint32_t);
    void (*wake)(void), (*sample)(void);
    sensor_inspect_fn_t inspect;
    bool (*announce)(uint8_t ch, const ambit_device_info_t *, const void *cal);
} sensor_inventory_ports_t;

void sensor_inventory_init(const sensor_inventory_ports_t *ports, uint64_t boot_id);
bool sensor_inventory_ready(void);
void sensor_inventory_wake(void);
/* UINT32_MAX means no safe work is due; ownership release wakes the worker. */
uint32_t sensor_inventory_wait_ms(void);
void sensor_inventory_refresh(void);
bool sensor_inventory_snapshot(sensor_snapshot_t *out);
bool sensor_inventory_cached(uint8_t ch, ambit_device_info_t *out);
bool sensor_inventory_accept(uint8_t ch, uint8_t type, const void *bytes, size_t size);
void sensor_inventory_announce_pending(uint8_t ch);
void sensor_inventory_config(uint8_t ch, const uint8_t *gains, const uint8_t *currents);
void sensor_inventory_invalidate(uint8_t ch);
void sensor_inventory_reset(void);
/* These reservations exclude refresh, not acquisition's own poll/fetch. A
 * timeout after issuing a run keeps its busy bit until completion or reset. */
bool sensor_inventory_acquire(uint8_t ch);
void sensor_inventory_complete(uint8_t ch);
void sensor_inventory_veto_begin(void);
void sensor_inventory_veto_end(void);
bool sensor_inventory_legacy_read_begin(void);
void sensor_inventory_legacy_read_end(void);
bool sensor_inventory_busy(void);
bool sensor_inventory_refresh_owned(void);
void sensor_inventory_note_stdout(uint32_t us);
void sensor_inventory_note_pin(uint32_t allocated_bytes);
void sensor_inventory_note_job_wait(uint32_t us);
uint32_t sensor_inventory_crc32(const void *bytes, size_t size);

typedef bool (*sensor_json_sink_t)(void *context, const void *bytes, size_t size);
/* Identical deterministic serializer for plain output, SHA and page slices. */
bool sensor_snapshot_json(const sensor_snapshot_t *, sensor_json_sink_t, void *context);
size_t sensor_snapshot_slice(const sensor_snapshot_t *, size_t offset, void *out,
                             size_t capacity, size_t *total);
