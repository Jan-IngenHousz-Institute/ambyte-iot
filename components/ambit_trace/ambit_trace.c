#include "ambit_trace.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "ambit_trace"

/* Maximum non-payload bytes in the v2 TSV line: 113-B fixed scalar/header
 * fields, 543-B NUL-excluded cmd, 1,535-B metadata, and two framing bytes.
 * Keep these named beside the producer so future cap changes fail at compile
 * time instead of drifting from event_log's admission rule. */
#define AMBIT_EVENT_FIXED_HEADER_MAX 113U
#define AMBIT_EVENT_FRAMING_MAX        2U
#define AMBIT_RUN_PAYLOAD_MAX  (AMBIT_RUN_PAYLOAD_CAP - 1U)
#define AMBIT_RUN_METADATA_MAX (AMBIT_RUN_METADATA_CAP - 1U)
#define AMBIT_CMD_ASCII_MAX    (AMBIT_CMD_ASCII_CAP - 1U)
#define AMBIT_RUN_RECORD_MAX \
    (AMBIT_RUN_PAYLOAD_MAX + AMBIT_RUN_METADATA_MAX + AMBIT_CMD_ASCII_MAX + \
     AMBIT_EVENT_FIXED_HEADER_MAX + AMBIT_EVENT_FRAMING_MAX)

_Static_assert(AMBIT_RUN_RECORD_MAX < EVLOG_RECORD_CAP_NORMAL,
               "AMBIT v2 fallback record must fit the normal event-log cap");
_Static_assert(EVLOG_RECORD_CAP_NORMAL - AMBIT_RUN_RECORD_MAX > 0U,
               "AMBIT v2 fallback record must retain positive admission margin");
_Static_assert(PAYLOAD_V3_MAX_ARRAYS == UART_SENSOR_MAX_ARRAYS,
               "payload array model must track the UART response model");
_Static_assert(PAYLOAD_V3_MAX_SEGMENTS == 16U,
               "AMBIT arrun protocol limit must track the payload model");

/* Reserved once, while the heap is still contiguous, and reused for every
 * trace. One reservation owns two non-overlapping regions under the mutex:
 * [0,63000) trace data and [63000,64536) fallback metadata. event_log consumes
 * both synchronously before unlock, so neither aliases live bytes or escapes
 * its ownership window. The buffer is shared by the main Lua task, lua exec,
 * and the future schedule runner, hence the lazily published mutex. */
static char *s_payload;
static SemaphoreHandle_t s_payload_mtx;
static portMUX_TYPE s_payload_mtx_init_lock = portMUX_INITIALIZER_UNLOCKED;

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static cmd_result_t trace_result(esp_err_t status, const char *fmt, ...)
{
    cmd_result_t result = {.status = status};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(result.message, sizeof result.message, fmt, ap);
    va_end(ap);
    return result;
}

static void payload_mtx_ensure(void)
{
    if (s_payload_mtx != NULL) return;
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    if (m == NULL) return; /* caller fails closed; shared bytes are never unlocked */
    taskENTER_CRITICAL(&s_payload_mtx_init_lock);
    if (s_payload_mtx == NULL) {
        s_payload_mtx = m;
        m = NULL;
    }
    taskEXIT_CRITICAL(&s_payload_mtx_init_lock);
    if (m != NULL) vSemaphoreDelete(m); /* lost the race; ours is surplus */
}

/* Caller must hold s_payload_mtx. Keeping the NULL check, allocation, and
 * publication in one locked helper prevents a registering exec state from
 * replacing the reservation while another state builds/stores a record. */
static char *payload_reserve_locked(void)
{
    if (s_payload == NULL) {
        char *candidate = malloc(AMBIT_RUN_BUFFER_CAP);
        if (candidate == NULL) return NULL;
        s_payload = candidate;
    }
    return s_payload;
}

esp_err_t ambit_trace_reserve(void)
{
    payload_mtx_ensure();
    if (s_payload_mtx == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_payload_mtx, portMAX_DELAY);
    char *payload = payload_reserve_locked();
    xSemaphoreGive(s_payload_mtx);
    return payload != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Resolve the cached AMBIT name for the event's `device` field, lazily fetching
 * identity (which also emits the once-per-connection DEVICE_INFO event). Fill
 * *info for v3 protocol/device fields; use the generic name until discovery. */
const char *ambit_device_name(uint8_t ch, ambit_device_info_t *info)
{
    cmd_ambit_device_info(ch, info);
    return (info->valid && info->ambit_name[0] != '\0') ? info->ambit_name : "ambit";
}

/* Write per-measurement config key/value pairs without surrounding braces so a
 * caller can splice them into metadata. cal_version is always present, which
 * lets optional gains/currents prepend their comma without another branch. */
int ambit_config_kvs(const ambit_device_info_t *info, char *buf, size_t cap)
{
    int o = 0;
    if (info->valid)
        o += snprintf(buf, cap, "\"cal_version\":\"%08lx\"", (unsigned long)info->cal_version);
    else
        o += snprintf(buf, cap, "\"cal_version\":null");
    if (info->gains_set && o > 0 && o < (int)cap)
        o += snprintf(buf + o, cap - o, ",\"gains\":[%u,%u,%u,%u,%u,%u]",
                      info->gains[0], info->gains[1], info->gains[2],
                      info->gains[3], info->gains[4], info->gains[5]);
    if (info->currents_set && o > 0 && o < (int)cap)
        o += snprintf(buf + o, cap - o, ",\"currents\":[%u,%u,%u]",
                      info->currents[0], info->currents[1], info->currents[2]);
    return o;
}

/* Fused store for small typed AMBIT queries: one MEASUREMENT event on uart_<ch>,
 * with cmd_raw expressed in the AMBIT's own ASCII vocabulary. */
int64_t ambit_store_small(uint8_t ch, const char *device, const char *cmd_ascii,
                          const char *metadata_json, int64_t start_ms,
                          int64_t end_ms, const char *payload_json)
{
    char chan[12];
    snprintf(chan, sizeof chan, "uart_%u", (unsigned)ch);
    int64_t mid = 0;
    if (cmd_next_measure_id(&mid).status != ESP_OK) return -1;
    measurement_event_desc_t d = {
        .measure_id = mid,
        .channel = chan,
        .device = device,
        .tag = MEASUREMENT_TAG_MEASUREMENT,
        .cmd_raw = cmd_ascii,
        .start_ms = start_ms,
        .end_ms = end_ms,
        .metadata_json = metadata_json,
        .payload_json = payload_json,
    };
    return cmd_store_event(&d).status == ESP_OK ? mid : -1;
}

/* Actinic value -> AMBIT LED-current byte, matching the original WRENCH ambyte
 * protocol generator: -255..-1 is an exact DAC level, 1..9999 is PAR in umol
 * converted by calibration and clamped to 4..255, and all other values are off. */
uint8_t ambit_actinic_to_dac(int16_t actinic, float par_coef)
{
    if (actinic < 0 && actinic > -256) return (uint8_t)(-actinic);
    if (actinic > 0 && actinic < 10000) {
        float t = par_coef * (float)actinic;
        if (t < 4.0f) return 4;
        if (t > 255.0f) return 255;
        return (uint8_t)t;
    }
    return 0;
}

void ambit_decode_segments(const uint8_t *run_arr, size_t nseg,
                           payload_v3_segment_t *segments)
{
    for (size_t i = 0; i < nseg; ++i) {
        const uint8_t *line = run_arr + i * 8U;
        segments[i] = (payload_v3_segment_t) {
            .type = line[0],
            .pulses = (uint16_t)(((uint16_t)line[2] << 8) | line[3]),
            .freq = (uint16_t)(((uint16_t)line[4] << 8) | line[5]),
            .actinic = line[6],
            .subsampling = line[7],
        };
    }
}

/* Reconstruct the AMBIT console grammar (arrun <len>,<persist>,<bytes...>).
 * This literal command becomes cmd_raw, so every trace carries its arguments. */
void ambit_build_cmd_ascii(char *out, size_t cap, const uint8_t *run_arr,
                           size_t nseg, uint8_t persist)
{
    int off = snprintf(out, cap, "arrun %u,%u", (unsigned)nseg, (unsigned)persist);
    for (size_t i = 0; i < nseg * 8U && off > 0 && off < (int)cap; ++i) {
        off += snprintf(out + off, cap - (size_t)off, ",%u", (unsigned)run_arr[i]);
    }
}

/* Build a caller-freed nseg*8 wire array. Calibration lookup deliberately stays
 * here so Lua and schedule callers cannot encode the same protocol differently. */
uint8_t *ambit_trace_build_run_arr(const ambit_trace_segment_t *segments,
                                   size_t nseg, uint8_t ch)
{
    if (segments == NULL || nseg == 0U || nseg > PAYLOAD_V3_MAX_SEGMENTS ||
        ch >= UART_SENSOR_NUM_CHANNELS) {
        return NULL;
    }
    uint8_t *run_arr = malloc(nseg * 8U);
    if (run_arr == NULL) return NULL;

    /* This AMBIT's PAR->DAC actinic coefficient is lazily fetched once and
     * cached (cmd 33). Fall back to 0.05 byte/umol if calibration is unreadable. */
    ambit_device_info_t info;
    cmd_ambit_device_info(ch, &info);
    const float par_coef = info.valid && info.actinic_coef > 0.0f
        ? info.actinic_coef : 0.05f;

    for (size_t i = 0; i < nseg; ++i) {
        uint8_t *line = run_arr + i * 8U;
        line[0] = segments[i].type;
        line[1] = segments[i].far_red ? 1U : 0U;
        line[2] = (uint8_t)(segments[i].pulses >> 8);
        line[3] = (uint8_t)segments[i].pulses;
        line[4] = (uint8_t)(segments[i].freq >> 8);
        line[5] = (uint8_t)segments[i].freq;
        line[6] = ambit_actinic_to_dac(segments[i].actinic, par_coef);
        line[7] = segments[i].subsampling;
    }
    return run_arr;
}

int64_t ambit_trace_estimate_ms(const ambit_trace_segment_t *segments, size_t nseg)
{
    if (segments == NULL) return 0;
    double total = 0.0;
    for (size_t i = 0; i < nseg; ++i) {
        const uint16_t freq = segments[i].freq > 0U ? segments[i].freq : 1U;
        total += ((double)segments[i].pulses / (double)freq) * 1000.0 + 300.0;
    }
    return (int64_t)total;
}

/* Decode the binary FSM response and optionally store one event. The synchronous
 * run and async trigger/fetch pair are the same stimulus, so both pass the same
 * reconstructed arrun command and converge on this single persistence route. */
esp_err_t ambit_trace_decode_store(uart_sensor_response_t *resp, uint8_t ch,
                                   bool store, int64_t start_ms, int64_t end_ms,
                                   const ambit_protocol_ref_t *protocol_ref,
                                   const char *cmd,
                                   const payload_v3_segment_t *segments,
                                   size_t segment_count,
                                   ambit_trace_result_t *out)
{
    if (resp == NULL) return ESP_ERR_INVALID_ARG;
    if (out == NULL || ch >= UART_SENSOR_NUM_CHANNELS || cmd == NULL ||
        segments == NULL || segment_count == 0U ||
        segment_count > PAYLOAD_V3_MAX_SEGMENTS) {
        uart_sensor_response_free(resp);
        return ESP_ERR_INVALID_ARG;
    }

    *out = (ambit_trace_result_t) {.measure_id = -1};
    const uint8_t narr = resp->array_count;
    if (narr == 0U) {
        uart_sensor_response_free(resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->array_count = narr;
    for (uint8_t a = 0; a < narr; ++a) {
        const uart_data_array_t *arr = &resp->arrays[a];
        if (arr->index == 1U) out->points = arr->length;
        if (arr->index == 0U && arr->length > 0U) {
            out->leaf_temp = (int16_t)(arr->data[0] & 0xFFFFU) / 100.0;
        }
    }

    /* Preserve the historical reservation point even for store=false: the Lua
     * binding has always proved the large trace buffer is available before it
     * reports a decoded run. Serialize build + synchronous store when used. */
    payload_mtx_ensure();
    if (s_payload_mtx == NULL) {
        uart_sensor_response_free(resp);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_payload_mtx, portMAX_DELAY);
    char *payload = payload_reserve_locked();
    if (payload == NULL) {
        xSemaphoreGive(s_payload_mtx);
        ESP_LOGE(TAG, "out of memory reserving %uB payload (free=%u, largest=%u)",
                 (unsigned)AMBIT_RUN_BUFFER_CAP,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        uart_sensor_response_free(resp);
        return ESP_ERR_NO_MEM;
    }

    if (store) {
        char chan[12];
        snprintf(chan, sizeof chan, "uart_%u", (unsigned)ch);
        ambit_device_info_t info;
        const char *device = ambit_device_name(ch, &info);
        int64_t mid = 0;
        if (cmd_next_measure_id(&mid).status == ESP_OK) {
            payload_v3_array_t arrays[PAYLOAD_V3_MAX_ARRAYS];
            for (uint8_t a = 0; a < narr; ++a) {
                arrays[a] = (payload_v3_array_t) {
                    .index = resp->arrays[a].index,
                    .length = resp->arrays[a].length,
                    .values = resp->arrays[a].data,
                };
            }
            payload_v3_trace_input_t input = {
                .measure_id = mid,
                .channel = chan,
                .device = device,
                .sensor_id = info.valid ? info.device_id : NULL,
                .start_utc_ms = start_ms,
                .end_utc_ms = end_ms,
                .protocol_name = protocol_ref != NULL && protocol_ref->protocol[0]
                    ? protocol_ref->protocol : NULL,
                .protocol_id = protocol_ref != NULL && protocol_ref->protocol_id[0]
                    ? protocol_ref->protocol_id : NULL,
                .protocol_cmd = cmd,
                .segments = segments,
                .segment_count = segment_count,
                .calibration_present = info.valid && isfinite(info.tick_factor) &&
                                       info.tick_factor > 0.0f,
                .cal_version = info.cal_version,
                .tick_factor = info.tick_factor,
                .gains_present = info.gains_set,
                .currents_present = info.currents_set,
                .arrays = arrays,
                .array_count = narr,
            };
            memcpy(input.gains, info.gains, sizeof input.gains);
            memcpy(input.currents, info.currents, sizeof input.currents);

            char *fallback_metadata = payload + AMBIT_RUN_PAYLOAD_CAP;
            char build_error[96];
            const payload_trace_route_t route = payload_v3_build_trace_lossless(
                payload, AMBIT_RUN_PAYLOAD_CAP,
                fallback_metadata, AMBIT_RUN_METADATA_CAP,
                &input, build_error, sizeof build_error);
            if (route == PAYLOAD_TRACE_ROUTE_V2) {
                ESP_LOGW(TAG, "%s", build_error);
            } else if (route == PAYLOAD_TRACE_ROUTE_ERROR) {
                ESP_LOGE(TAG, "AMBIT payload build failed: %s", build_error);
            }

            measurement_event_desc_t desc = {
                .measure_id = mid,
                .channel = chan,
                .device = device,
                .tag = MEASUREMENT_TAG_MEASUREMENT,
                .cmd_raw = cmd,
                .start_ms = start_ms,
                .end_ms = end_ms,
                /* v3 owns payload_json directly. cmd_raw stays in its on-disk
                 * column for replay diagnostics; the publisher does not rebuild v2. */
                .metadata_json = route == PAYLOAD_TRACE_ROUTE_V2 && fallback_metadata[0]
                    ? fallback_metadata : NULL,
                .payload_json = payload,
            };
            /* Exactly one store attempt follows routing. Any ordinary v3
             * representability failure preserves completed arrays through v2. */
            if (route != PAYLOAD_TRACE_ROUTE_ERROR &&
                cmd_store_event(&desc).status == ESP_OK) {
                out->measure_id = mid;
            }
        }
    }

    xSemaphoreGive(s_payload_mtx);
    uart_sensor_response_free(resp);
    return ESP_OK;
}

cmd_result_t ambit_trace_trigger(uint8_t ch,
                                 const ambit_trace_segment_t *segments,
                                 size_t nseg,
                                 const ambit_trace_options_t *opts,
                                 ambit_trace_pending_t *pending)
{
    if (ch >= UART_SENSOR_NUM_CHANNELS || segments == NULL || pending == NULL ||
        nseg == 0U || nseg > PAYLOAD_V3_MAX_SEGMENTS) {
        return trace_result(ESP_ERR_INVALID_ARG, "invalid ambit trace trigger arguments");
    }

    pending->valid = false;
    const uint8_t persist = opts != NULL ? opts->persist : 0U;
    const bool allow_interrupt = opts != NULL && opts->allow_interrupt;
    const uint32_t timeout_ms = opts != NULL ? opts->timeout_ms : 3000U;
    pending->protocol_ref = opts != NULL
        ? opts->protocol_ref : (ambit_protocol_ref_t) {0};

    uint8_t *run_arr = ambit_trace_build_run_arr(segments, nseg, ch);
    if (run_arr == NULL) return trace_result(ESP_ERR_NO_MEM, "out of memory");
    ambit_decode_segments(run_arr, nseg, pending->segments);
    pending->segment_count = (uint8_t)nseg;
    ambit_build_cmd_ascii(pending->cmd, sizeof pending->cmd, run_arr, nseg, persist);
    pending->start_ms = now_ms();

    cmd_result_t result = cmd_ambit_trigger(ch, run_arr, (uint8_t)nseg, persist,
                                            allow_interrupt, timeout_ms);
    free(run_arr);
    if (result.status == ESP_OK) pending->valid = true;
    return result;
}

cmd_result_t ambit_trace_fetch(uint8_t ch, ambit_trace_pending_t *pending,
                               bool store, uint32_t timeout_ms,
                               ambit_trace_result_t *out)
{
    if (ch >= UART_SENSOR_NUM_CHANNELS || pending == NULL || out == NULL) {
        return trace_result(ESP_ERR_INVALID_ARG, "invalid ambit trace fetch arguments");
    }
    /* Do not issue cmd24 without a successfully retained trigger model: doing
     * so consumes sensor state before discovering the arrays cannot be attributed. */
    if (!payload_v3_can_fetch_retained(pending->valid, pending->segment_count)) {
        return trace_result(ESP_ERR_INVALID_STATE,
                            "no successful retained ambit.trigger for channel");
    }

    uart_sensor_response_t response;
    cmd_result_t result = cmd_ambit_fetch(ch, &response, timeout_ms);
    if (result.status != ESP_OK) {
        uart_sensor_response_free(&response);
        return result;
    }

    pending->valid = false;
    const esp_err_t err = ambit_trace_decode_store(
        &response, ch, store, pending->start_ms, now_ms(), &pending->protocol_ref,
        pending->cmd, pending->segments, pending->segment_count, out);
    if (err == ESP_ERR_INVALID_RESPONSE) {
        return trace_result(err, "ambit run returned no arrays");
    }
    if (err != ESP_OK) {
        return trace_result(err, "ambit trace decode/store failed: %s",
                            esp_err_to_name(err));
    }
    return trace_result(ESP_OK, "ambit trace fetched");
}
