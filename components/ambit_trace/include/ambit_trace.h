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
    /* All six fields are part of the AMBIT wire format. Encoder defaults are
     * type=2, far_red=false, subsampling=1. */
    uint8_t type;        /* 0 skip, 1 incl. 730-nm reflectance, 2 no IR. */
    bool far_red;        /* Byte 1; meaningful only when type == 1. */
    uint16_t pulses;
    uint16_t freq;
    int16_t actinic;     /* WRENCH convention; calibration converts it to DAC. */
    uint8_t subsampling; /* 0 none, 1 every pulse, 2 every 8 averaged. */
} ambit_trace_segment_t;

/* A schedule's optional `tag:` is a SECOND axis, not a rename. Until fw 2.0.0 it
 * was written straight into `protocol`, so every dark-edge trace published
 * protocol.name="edge" and the MPF protocol became unrecoverable from the
 * payload (2026-09 soak). Tags are short labels, so this does not need the
 * 256 B protocol caps. */
#define AMBIT_PROTOCOL_TAG_CAP 32U

typedef struct {
    char protocol[AMBIT_PROTOCOL_FIELD_CAP];
    char protocol_id[AMBIT_PROTOCOL_FIELD_CAP];
    char tag[AMBIT_PROTOCOL_TAG_CAP];
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
 * Safe to call more than once and from competing runner/CLI contexts. */
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

/* ── Run-time estimate: two acquisition engines, two time bases ──────────
 *
 * The scheduler uses the estimate only to defer polling and to bound a broken
 * AMBIT, never as a measurement clock — but it must be an UPPER bound of the
 * wall-clock run. The AMBIT executes a retained run (cmd 22) inline on its
 * serial loop: it cannot answer a poll (cmd 23) while measuring, and the wake
 * bytes a poll pushes into a light-sleeping AMBIT are mangled by the UART wake,
 * queued, and parsed only after the run, which can desynchronise the following
 * poll/fetch. So a poll must never land inside the run.
 *
 *  FREE_RUN  AMBIT fw < 1.4.0. The ADPD period counter paces the run; a nominal
 *            1 Hz pulse really takes tick_factor s (below 1 — 0.85…0.94 across
 *            shipped calibrations, which the trace decoder corrects for), so
 *            Σ pulses/freq already overshoots the run and 300 ms per segment of
 *            configuration/light-sleep slack is the only overhead. The shipped
 *            45-pulse 1 Hz SS: ~41.2 s measured, 45.3 s estimated.
 *  PACED     AMBIT fw >= 1.4.0 (EXT_SYNC engine, v1.4.0-rc.1, ambit PR #10).
 *            The ESP clock fires every pulse at exactly 1/freq, after ~0.5 s of
 *            per-run setup (environment read, ADPD reconfiguration, arm settle,
 *            three discarded warm-up sequences) plus a per-line reconfiguration.
 *            The same SS run takes ~44.4 s — past 90 % of the free-run estimate
 *            (40.8 s), which is how 2.5.2 gateways polled 1.4.0 AMBITs mid-run
 *            and lost channel 0's SS traces for hours (Nergena, 2026-10-05).
 *            Estimate: Σ pulses/freq + 500 ms per run + 100 ms per segment —
 *            45.6 s / 10.1 s for the shipped SS / MPF. Σ pulses/freq counts N
 *            periods for N edges spanning (N-1)/freq, so each segment carries
 *            one period of slack on top; that and the setup term are the guard.
 *            Array 9 (edge times) is additive; arrays 0–8, the wire bytes and
 *            the decoder are identical for both engines.
 *
 * The engine comes from the cached cmd 33/2 identity (cmd_ambit_device_info).
 * An UNKNOWN identity is treated as PACED: polling a free-run AMBIT a few
 * seconds late costs nothing, polling a paced one early loses the trace. */
typedef enum {
    AMBIT_TRACE_ENGINE_FREE_RUN = 0,
    AMBIT_TRACE_ENGINE_PACED,
} ambit_trace_engine_t;

#define AMBIT_TRACE_PACED_FW_MAJOR       1
#define AMBIT_TRACE_PACED_FW_MINOR       4
#define AMBIT_TRACE_FREE_RUN_SEGMENT_MS  300
#define AMBIT_TRACE_PACED_SETUP_MS       500
#define AMBIT_TRACE_PACED_SEGMENT_MS     100

/* NULL or an invalid identity → PACED (see above). */
ambit_trace_engine_t ambit_trace_engine_for(const ambit_device_info_t *info);
/* Upper bound of the wall-clock run for `engine`; 0 for no segments. The
 * runner uses ONE value per channel for both the poll start (>= 100 % of it)
 * and the broken-AMBIT deadline (it plus deadline_margin). */
int64_t ambit_trace_estimate_ms_for(const ambit_trace_segment_t *segments, size_t nseg,
                                    ambit_trace_engine_t engine);
/* The FREE_RUN estimate, for callers without a channel identity. */
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
