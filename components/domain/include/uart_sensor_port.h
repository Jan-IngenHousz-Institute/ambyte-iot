#ifndef AMBYTE_UART_SENSOR_PORT_H
#define AMBYTE_UART_SENSOR_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_err.h"
#include "uart_stream_support.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_SENSOR_NUM_CHANNELS  4
/* v2 run streams up to 8 arrays (ENV, Fluo, Fluoref, Sun, Leaf, 730, 730ref,
 * TIMING); keep headroom so the receive loop still reads CMD_END after the last. */
#define UART_SENSOR_MAX_ARRAYS    12

/* Sentinel value for expect_raw: return immediately after CMD_DONE.
 * Used for configuration commands (set gains, set currents, etc.) that
 * send CMD_DONE but no CMD_END and no response data. */
#define UART_QUERY_ACK_ONLY  ((size_t)-1)

typedef enum {
    UART_SENSOR_DISCONNECTED = 0,
    UART_SENSOR_CONNECTED    = 1,
    UART_SENSOR_BUSY         = 2,
} uart_sensor_state_t;

/* One data array received via FSM handshake (one per fsm_send_esp call on the
 * ambit side).  Caller frees via uart_sensor_response_free(). */
typedef struct {
    uint8_t   index;        /* array index (0–7) reported in the FSM length header */
    uint32_t *data;         /* heap-allocated uint32 array */
    uint16_t  length;       /* number of uint32_t elements */
} uart_data_array_t;

/* Response returned by a UART sensor query. */
typedef struct {
    /* Structured FSM data arrays (populated for run/measurement commands) */
    uart_data_array_t arrays[UART_SENSOR_MAX_ARRAYS];
    uint8_t           array_count;

    /* Raw response bytes (populated for simple immediate-response commands) */
    uint8_t          *raw;
    size_t            raw_len;

    esp_err_t         status;
} uart_sensor_response_t;

/* Free all heap-allocated fields inside a response struct. */
static inline void uart_sensor_response_free(uart_sensor_response_t *resp)
{
    if (resp == NULL) {
        return;
    }
    for (uint8_t i = 0; i < resp->array_count; i++) {
        free(resp->arrays[i].data);
        resp->arrays[i].data   = NULL;
        resp->arrays[i].length = 0;
    }
    resp->array_count = 0;
    free(resp->raw);
    resp->raw     = NULL;
    resp->raw_len = 0;
}

/* ---------------------------------------------------------------------------
 * Port function-pointer types
 * ---------------------------------------------------------------------------
 * uart_sensor_query_fn — send a binary ESP command and collect the response.
 *
 *   channel       : 0–3 (AMBIT1–AMBIT4)
 *   cmd           : 8-byte command array (the payload after the 0xA0 header)
 *   extra/extra_len: optional additional payload bytes (e.g. run-array config)
 *   expect_raw    : expected raw response length for immediate commands.
 *                   0 → FSM data-transfer mode (measurement commands).
 *                   >0 → read exactly this many bytes after CMD_DONE, then
 *                         verify CMD_END follows.
 *   response      : output — caller must call uart_sensor_response_free()
 *   timeout_ms    : overall timeout for the entire exchange
 */
typedef esp_err_t (*uart_sensor_query_fn)(uint8_t channel,
                                          const uint8_t cmd[8],
                                          const uint8_t *extra, size_t extra_len,
                                          size_t expect_raw,
                                          uart_sensor_response_t *response,
                                          uint32_t timeout_ms);

typedef esp_err_t (*uart_sensor_ping_fn)(uint8_t channel, bool *connected);

typedef esp_err_t (*uart_sensor_status_fn)(uint8_t channel,
                                           uart_sensor_state_t *out);

/* uart_sensor_text_query_fn — send an ASCII command and read one response line.
 *
 * Sends `cmd` followed by `terminator`, then accumulates incoming bytes until
 * `terminator` is received or `timeout_ms` elapses. The terminator itself is
 * stripped from `out_resp`. On timeout, *resp_len is 0 and the function
 * returns ESP_ERR_TIMEOUT (out_resp[0] is set to '\0').
 *
 *   channel      : 0–3
 *   cmd          : NUL-terminated ASCII command (no terminator appended yet)
 *   terminator   : line terminator string, used both for send framing and as
 *                  the receive delimiter (e.g. "\n" or "\r\n")
 *   out_resp     : caller-provided buffer, NUL-terminated on return
 *   resp_cap     : sizeof(out_resp); response is truncated at resp_cap-1
 *   resp_len     : OUT — bytes written to out_resp (excluding the NUL)
 *   timeout_ms   : total wall-clock budget for the send + read
 */
typedef esp_err_t (*uart_sensor_text_query_fn)(uint8_t channel,
                                               const char *cmd,
                                               const char *terminator,
                                               char       *out_resp,
                                               size_t      resp_cap,
                                               size_t     *resp_len,
                                               uint32_t    timeout_ms);

/* A stream sink consumes response bytes incrementally while the sensor-channel
 * lock is held. It must not call back into a sensor command. Each call is
 * bounded so a sink can make one record atomic; returning an error rejects that
 * whole call and aborts after all earlier calls remain delivered. */
typedef esp_err_t (*uart_sensor_stream_write_fn)(const uint8_t *data,
                                                 size_t len,
                                                 void *ctx);

/* uart_sensor_stream_query_fn — send an ASCII command and relay MANY response
 * lines until a line containing `sentinel` (e.g. "Done" or an openJII frame
 * token) arrives or `timeout_ms` elapses. Pre-wakes the port. Bytes, including
 * the terminating newline, are passed to `write` in order rather than collected
 * in one response-sized allocation; *out_len is the number accepted by the
 * sink. No callback exceeds UART_STREAM_CHUNK_MAX bytes. `deadline_us` is an
 * already-started absolute wall-clock deadline shared with outer arbitration. */
typedef esp_err_t (*uart_sensor_stream_query_fn)(uint8_t channel,
                                                 const char *cmd,
                                                 const char *terminator,
                                                 const char *sentinel,
                                                 uart_sensor_stream_write_fn write,
                                                 void       *write_ctx,
                                                 size_t     *out_len,
                                                 int64_t     deadline_us);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_UART_SENSOR_PORT_H */
