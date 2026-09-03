#ifndef AMBYTE_AMBIT_TRACE_H
#define AMBYTE_AMBIT_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_commands.h"
#include "esp_err.h"
#include "payload_v3.h"
#include "uart_sensor_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max length of a reconstructed "arrun" ASCII command: "arrun " + nseg(<=2)
 * + "," + persist(<=3) + "," + 16 segments x 8 bytes x "255," (<=4),
 * with headroom for the terminating NUL. */
#define AMBIT_CMD_ASCII_CAP 544U
/* The raw v2 fallback carries metadata in the event-log column. Reserve enough
 * room for that complete record, rather than letting a completed run fit the
 * v3 buffer and then become unstorable only because it needs fallback. This
 * deliberately reduces the effective payload cap from 63,999 to 62,999 bytes. */
#define AMBIT_RUN_PAYLOAD_CAP  63000U
#define AMBIT_RUN_METADATA_CAP  1536U
#define AMBIT_RUN_BUFFER_CAP \
    (AMBIT_RUN_PAYLOAD_CAP + AMBIT_RUN_METADATA_CAP)

#define AMBIT_PROTOCOL_FIELD_CAP 256U

typedef struct {
    /* All six fields are part of the AMBIT wire format. Defaults preserve the
     * former Lua encoder: type=2, far_red=false, subsampling=1. */
    uint8_t type;        /* 0 skip, 1 incl. 730-nm reflectance, 2 no IR. */
    bool far_red;        /* Byte 1; meaningful only when type == 1. */
    uint16_t pulses;
    uint16_t freq;
    int16_t actinic;     /* WRENCH convention; calibration converts it to DAC. */
    uint8_t subsampling; /* 0 none, 1 every pulse, 2 every 8 averaged. */
} ambit_trace_segment_t;

typedef struct {
    char protocol[AMBIT_PROTOCOL_FIELD_CAP];
    char protocol_id[AMBIT_PROTOCOL_FIELD_CAP];
} ambit_protocol_ref_t;

typedef struct {
    uint8_t persist;
    bool allow_interrupt;
    uint32_t timeout_ms;
    ambit_protocol_ref_t protocol_ref;
} ambit_trace_options_t;

/* Caller-owned attribution retained between an async trigger and fetch. Keep
 * one per UART channel; `valid` is set only after the AMBIT acknowledges the
 * trigger and is cleared once a successful fetch consumes the sensor result. */
typedef struct {
    bool valid;
    uint8_t segment_count;
    int64_t start_ms;
    ambit_protocol_ref_t protocol_ref;
    char cmd[AMBIT_CMD_ASCII_CAP];
    payload_v3_segment_t segments[PAYLOAD_V3_MAX_SEGMENTS];
} ambit_trace_pending_t;

typedef struct {
    size_t points;
    double leaf_temp;
    int64_t measure_id; /* -1 when store=false or the store attempt failed. */
    uint8_t array_count;
} ambit_trace_result_t;

/* Reserve the shared trace/fallback buffer while the heap is contiguous.
 * Safe to call more than once and from competing Lua/CLI execution contexts. */
esp_err_t ambit_trace_reserve(void);

const char *ambit_device_name(uint8_t ch, ambit_device_info_t *info);
int ambit_config_kvs(const ambit_device_info_t *info, char *buf, size_t cap);
int64_t ambit_store_small(uint8_t ch, const char *device, const char *cmd_ascii,
                          const char *metadata_json, int64_t start_ms,
                          int64_t end_ms, const char *payload_json);

uint8_t ambit_actinic_to_dac(int16_t actinic, float par_coef);
void ambit_decode_segments(const uint8_t *run_arr, size_t nseg,
                           payload_v3_segment_t *segments);
void ambit_build_cmd_ascii(char *out, size_t cap, const uint8_t *run_arr,
                           size_t nseg, uint8_t persist);
uint8_t *ambit_trace_build_run_arr(const ambit_trace_segment_t *segments,
                                   size_t nseg, uint8_t ch);

/* Approximate run time from main.lua: pulses/freq seconds plus 300 ms of
 * per-segment configuration/light-sleep slack. The scheduler uses the estimate
 * only to defer polling and bound a broken AMBIT, not as a measurement clock. */
int64_t ambit_trace_estimate_ms(const ambit_trace_segment_t *segments, size_t nseg);

/* Consumes `resp` on every path, including errors. A failed optional store is
 * reported as measure_id=-1 while the successfully decoded trace remains OK. */
esp_err_t ambit_trace_decode_store(uart_sensor_response_t *resp, uint8_t ch,
                                   bool store, int64_t start_ms, int64_t end_ms,
                                   const ambit_protocol_ref_t *protocol_ref,
                                   const char *cmd,
                                   const payload_v3_segment_t *segments,
                                   size_t segment_count,
                                   ambit_trace_result_t *out);

cmd_result_t ambit_trace_trigger(uint8_t ch,
                                 const ambit_trace_segment_t *segments,
                                 size_t nseg,
                                 const ambit_trace_options_t *opts,
                                 ambit_trace_pending_t *pending);
cmd_result_t ambit_trace_fetch(uint8_t ch, ambit_trace_pending_t *pending,
                               bool store, uint32_t timeout_ms,
                               ambit_trace_result_t *out);

#ifdef __cplusplus
}
#endif

#endif
