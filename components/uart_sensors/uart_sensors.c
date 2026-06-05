/*
 * uart_sensors.c — 4-channel UART sensor driver (Option D: GPIO remap)
 *
 * Implements the ambit-1 binary ESP protocol:
 *   Wake (0xAA) → Ack (0x80) → Header (0xA0) + 8-byte cmd [+ extra]
 *   → CMD_DONE (0xA1) → response data → CMD_END (0xF0)
 *
 * For measurement commands (expect_raw == 0) the response arrives via the
 * ambit-side FSM data-transfer handshake (wake 0xD3, ack 0xD2, length header,
 * binary arrays, checksums).
 *
 * Channels 0–1 use dedicated UART controllers (UART1, UART2).
 * Channels 2–3 share UART0 with GPIO-matrix pin remap between accesses.
 */

#include "uart_sensors.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "uart_sens"

/* ── Ambit-1 protocol constants ────────────────────────────────────── */

#define AMBIT_WAKE          0xAA   /* 170 — host → ambit wake signal */
#define AMBIT_ACK           0x80   /* 128 — ambit → host wake ack    */
#define AMBIT_CMD_HEADER    0xA0   /* 160 — command frame start      */
#define AMBIT_CMD_DONE      0xA1   /* 161 — command accepted         */
#define AMBIT_CMD_END       0xF0   /* 240 — end of response          */
#define AMBIT_BOOT_IDLE     0x85   /* 133 — ambit boot/wake idle     */

/* FSM data-transfer constants (ambit → host) */
#define FSM_WAKE            0xD3   /* 211 — ambit wake-for-data      */
#define FSM_AWAKE           0xD2   /* 210 — host ack                 */
#define FSM_DATA_HDR        0xD4   /* 212 — data header marker       */
#define FSM_LEN_MARKER      0x96   /* 150 — inside length header     */
#define FSM_READY           0xC8   /* 200 — host ready for array     */
#define FSM_DATA_PASS       0xB4   /* 180 — host data-received OK    */
#define FSM_RESET           0xDE   /* 222 — host requests reset      */

/* ── Driver configuration ──────────────────────────────────────────── */

#define UART_BAUD_RATE      115200
#define UART_RX_BUF_SIZE    2048
#define WAKE_RETRIES        25
#define WAKE_RETRY_MS       50
#define PING_TIMEOUT_MS     2000
#define PING_CACHE_TTL_US   (10LL * 1000 * 1000)  /* 10 s */

/* ── Per-channel descriptor ────────────────────────────────────────── */

typedef struct {
    uart_port_t         uart_num;
    int                 rx_pin;
    int                 tx_pin;
    bool                shared;      /* channels sharing UART0 */
    uart_sensor_state_t state;
    int64_t             ping_ts;     /* last ping timestamp (µs) */
    bool                ping_ok;     /* cached ping result */
} channel_t;

static channel_t s_ch[UART_SENSOR_NUM_CHANNELS];

static SemaphoreHandle_t s_uart0_mtx;       /* guards UART0 remap */
static SemaphoreHandle_t s_ch_mtx[UART_SENSOR_NUM_CHANNELS];
static bool s_inited;

/* ── Helpers ───────────────────────────────────────────────────────── */

static inline int64_t now_us(void) { return esp_timer_get_time(); }

static inline bool deadline_reached(int64_t deadline)
{
    return now_us() >= deadline;
}

/* Read exactly `len` bytes from UART, blocking up to the deadline.
 * Returns ESP_OK or ESP_ERR_TIMEOUT. */
static esp_err_t uart_read_exact(uart_port_t port, uint8_t *buf, size_t len,
                                 int64_t deadline)
{
    size_t got = 0;
    while (got < len) {
        int64_t remain_us = deadline - now_us();
        if (remain_us <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remain_us / 1000));
        if (ticks == 0) {
            ticks = 1;
        }
        int n = uart_read_bytes(port, buf + got, len - got, ticks);
        if (n > 0) {
            got += (size_t)n;
        }
    }
    return ESP_OK;
}

/* Scan UART RX for a single target byte, discarding everything else.
 * Returns ESP_OK when found, ESP_ERR_TIMEOUT otherwise. */
static esp_err_t uart_scan_byte(uart_port_t port, uint8_t target,
                                int64_t deadline)
{
    while (!deadline_reached(deadline)) {
        uint8_t b;
        int64_t remain = deadline - now_us();
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remain > 0 ? remain / 1000 : 1));
        if (ticks == 0) {
            ticks = 1;
        }
        int n = uart_read_bytes(port, &b, 1, ticks);
        if (n == 1 && b == target) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

/* Write a single byte to UART TX. */
static void uart_write_byte(uart_port_t port, uint8_t b)
{
    uart_write_bytes(port, (const char *)&b, 1);
}

/* ── Acquire / release channel (+ UART0 remap if shared) ──────────── */

static esp_err_t channel_acquire(uint8_t ch, TickType_t wait)
{
    if (xSemaphoreTake(s_ch_mtx[ch], wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_ch[ch].shared) {
        if (xSemaphoreTake(s_uart0_mtx, wait) != pdTRUE) {
            xSemaphoreGive(s_ch_mtx[ch]);
            return ESP_ERR_TIMEOUT;
        }
        uart_flush_input(UART_NUM_0);
        uart_set_pin(UART_NUM_0, s_ch[ch].tx_pin, s_ch[ch].rx_pin,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        ESP_LOGD(TAG, "UART0 remapped to ch%u (TX=%d RX=%d)",
                 ch, s_ch[ch].tx_pin, s_ch[ch].rx_pin);
    }
    s_ch[ch].state = UART_SENSOR_BUSY;
    return ESP_OK;
}

static void channel_release(uint8_t ch)
{
    if (s_ch[ch].shared) {
        xSemaphoreGive(s_uart0_mtx);
    }
    /* State is set by the caller before release (CONNECTED or DISCONNECTED).
     * Only clear BUSY if the caller forgot to set a final state. */
    if (s_ch[ch].state == UART_SENSOR_BUSY) {
        s_ch[ch].state = UART_SENSOR_DISCONNECTED;
    }
    xSemaphoreGive(s_ch_mtx[ch]);
}

/* ── Ambit wake sequence ───────────────────────────────────────────── */

static esp_err_t ambit_wake(uart_port_t port, int64_t deadline)
{
    uart_flush_input(port);

    for (int attempt = 0; attempt < WAKE_RETRIES && !deadline_reached(deadline); attempt++) {
        /* Send 3 wake bytes (ambit light-sleep UART threshold = 3 edges) */
        uint8_t wake3[3] = { AMBIT_WAKE, AMBIT_WAKE, AMBIT_WAKE };
        uart_write_bytes(port, (const char *)wake3, sizeof(wake3));

        int64_t retry_deadline = now_us() + (int64_t)WAKE_RETRY_MS * 1000;
        if (retry_deadline > deadline) {
            retry_deadline = deadline;
        }
        /* Scan for ack (128) or boot-idle (133, means ambit just woke) */
        while (!deadline_reached(retry_deadline)) {
            uint8_t b;
            int n = uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(20));
            if (n == 1) {
                if (b == AMBIT_ACK) {
                    return ESP_OK;
                }
                if (b == AMBIT_BOOT_IDLE) {
                    /* Ambit just woke from sleep — ack should follow soon or
                     * we resend wake on next iteration. */
                    ESP_LOGD(TAG, "got boot-idle, retrying wake");
                }
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

/* ── Send command (header + 8-byte cmd + optional extra) ───────────── */

static esp_err_t ambit_send_cmd(uart_port_t port, const uint8_t cmd[8],
                                const uint8_t *extra, size_t extra_len)
{
    uart_write_byte(port, AMBIT_CMD_HEADER);
    uart_write_bytes(port, (const char *)cmd, 8);
    if (extra != NULL && extra_len > 0) {
        uart_write_bytes(port, (const char *)extra, extra_len);
    }
    return ESP_OK;
}

/* ── Receive one FSM data array (host side of ambit's fsm_send_esp) ── */

static esp_err_t fsm_receive_one_array(uart_port_t port,
                                       uart_data_array_t *out,
                                       int64_t deadline)
{
    /* ---- Step 1: ack wake (we already consumed the 211 byte) ---- */
    uart_write_byte(port, FSM_AWAKE);       /* 210 — ack wake */

    /* ---- Step 2: send 210 again for length phase ---- */
    vTaskDelay(pdMS_TO_TICKS(5));
    uart_write_byte(port, FSM_AWAKE);       /* 210 — ready for length */

    /* ---- Step 3: scan for length header {212, 150, ...} ---- */
    int64_t hdr_deadline = now_us() + 2000000LL; /* 2 s max for length step */
    if (hdr_deadline > deadline) {
        hdr_deadline = deadline;
    }

    uint8_t hdr[8];
    memset(hdr, 0, sizeof(hdr));
    bool hdr_found = false;
    while (!deadline_reached(hdr_deadline)) {
        uint8_t b;
        int n = uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(50));
        if (n != 1) {
            continue;
        }
        if (b != FSM_DATA_HDR) {
            continue;                       /* skip debug / echo bytes */
        }
        hdr[0] = FSM_DATA_HDR;
        esp_err_t err = uart_read_exact(port, hdr + 1, 7, hdr_deadline);
        if (err != ESP_OK) {
            return err;
        }
        if (hdr[1] != FSM_LEN_MARKER) {
            continue;                       /* stray 212, keep scanning */
        }
        hdr_found = true;
        break;
    }
    if (!hdr_found) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t arr_idx = hdr[2];
    uint16_t arr_len = ((uint16_t)hdr[3] << 8) | hdr[4];

    /* Verify header checksum */
    uint8_t cs = 0;
    for (int i = 0; i < 7; i++) {
        cs += hdr[i];
    }
    if (cs != hdr[7]) {
        ESP_LOGW(TAG, "FSM header checksum mismatch (got 0x%02X, want 0x%02X)",
                 hdr[7], cs);
        uart_write_byte(port, FSM_RESET);
        return ESP_ERR_INVALID_CRC;
    }

    /* ---- Step 4: send ready for data ---- */
    uart_write_byte(port, FSM_READY);       /* 200 */
    vTaskDelay(pdMS_TO_TICKS(5));
    uart_write_byte(port, FSM_READY);       /* 200 again for data phase */

    /* ---- Step 5: read binary data (arr_len * 4 bytes) ---- */
    if (arr_len == 0) {
        out->index  = arr_idx;
        out->data   = NULL;
        out->length = 0;
        uart_write_byte(port, FSM_DATA_PASS);
        return ESP_OK;
    }

    uint32_t *data = malloc((size_t)arr_len * sizeof(uint32_t));
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Retry loop for data phase — ambit resends on 200 (up to 6 checksum errors) */
    for (int data_attempt = 0; data_attempt < 4; data_attempt++) {
        int64_t data_deadline = now_us() + 10000000LL;  /* 10 s per attempt */
        if (data_deadline > deadline) {
            data_deadline = deadline;
        }

        /* Send 200 to tell ambit we are ready for (re)send */
        if (data_attempt > 0) {
            uart_write_byte(port, FSM_READY);   /* 200 = resend */
            /* Ambit's fsm_send_data waits for 200 before sending */
        }

        esp_err_t err = uart_read_exact(port, (uint8_t *)data,
                                        (size_t)arr_len * 4, data_deadline);
        if (err != ESP_OK) {
            free(data);
            return err;
        }

        /* ---- Step 6: read trailer {212, 0, 0, checksum} ---- */
        uint8_t trailer[4];
        err = uart_read_exact(port, trailer, 4, data_deadline);
        if (err != ESP_OK) {
            free(data);
            return err;
        }

        /* Verify data checksum: sum of every data byte mod 256. The ambit's
         * dataclass::send byte-sums to match (u32_byte_sum over each element). */
        uint8_t data_cs = 0;
        const uint8_t *raw_bytes = (const uint8_t *)data;
        for (size_t i = 0; i < (size_t)arr_len * 4; i++) {
            data_cs += raw_bytes[i];
        }
        if (data_cs == trailer[3]) {
            /* ---- Step 7: ack data pass ---- */
            uart_write_byte(port, FSM_DATA_PASS);   /* 180 */

            out->index  = arr_idx;
            out->data   = data;
            out->length = arr_len;
            return ESP_OK;
        }
        ESP_LOGW(TAG, "FSM data checksum mismatch for array %u (attempt %d)",
                 arr_idx, data_attempt + 1);
    }

    /* All retries exhausted */
    free(data);
    uart_write_byte(port, FSM_RESET);       /* 222 = give up on this array */
    return ESP_ERR_INVALID_CRC;
}

/* ── Receive full FSM response (multiple arrays) ───────────────────── */

static esp_err_t receive_fsm_response(uart_port_t port,
                                      uart_sensor_response_t *resp,
                                      int64_t deadline)
{
    /* We arrive here after CMD_DONE.  Scan for FSM_WAKE or CMD_END. */
    while (resp->array_count < UART_SENSOR_MAX_ARRAYS &&
           !deadline_reached(deadline)) {
        uint8_t b;
        int n = uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(500));
        if (n != 1) {
            continue;
        }

        if (b == AMBIT_CMD_END) {
            resp->status = ESP_OK;
            return ESP_OK;
        }

        if (b == FSM_WAKE) {
            esp_err_t err = fsm_receive_one_array(
                port, &resp->arrays[resp->array_count], deadline);
            if (err == ESP_OK) {
                resp->array_count++;
            } else if (err == ESP_ERR_INVALID_CRC) {
                ESP_LOGW(TAG, "FSM array checksum error, ambit may retry");
                /* Ambit will retry the same array (its state resets to WAKEUPCALLS) */
            } else {
                resp->status = err;
                return err;
            }
        }
        /* Other bytes (debug output, echoes) are silently discarded */
    }

    resp->status = ESP_ERR_TIMEOUT;
    return ESP_ERR_TIMEOUT;
}

/* ── Receive immediate response (fixed-length raw bytes) ───────────── */

static esp_err_t receive_raw_response(uart_port_t port,
                                      uart_sensor_response_t *resp,
                                      size_t expect_len,
                                      int64_t deadline)
{
    uint8_t *buf = malloc(expect_len);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = uart_read_exact(port, buf, expect_len, deadline);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    /* Verify CMD_END follows */
    err = uart_scan_byte(port, AMBIT_CMD_END, deadline);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CMD_END not received after raw data");
        /* Keep the data anyway — it was received successfully */
    }

    resp->raw     = buf;
    resp->raw_len = expect_len;
    resp->status  = ESP_OK;
    return ESP_OK;
}

/* ── Public: full query (wake → cmd → response) ───────────────────── */

static esp_err_t do_query(uint8_t channel,
                          const uint8_t cmd[8],
                          const uint8_t *extra, size_t extra_len,
                          size_t expect_raw,
                          uart_sensor_response_t *response,
                          uint32_t timeout_ms)
{
    if (!s_inited || channel >= UART_SENSOR_NUM_CHANNELS) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(response, 0, sizeof(*response));

    esp_err_t err = channel_acquire(channel, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        response->status = err;
        return err;
    }

    uart_port_t port = s_ch[channel].uart_num;
    int64_t deadline = now_us() + (int64_t)timeout_ms * 1000;

    /* 1. Wake */
    err = ambit_wake(port, deadline);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ch%u: wake failed", channel);
        s_ch[channel].state = UART_SENSOR_DISCONNECTED;
        goto out;
    }

    /* 2. Send command */
    err = ambit_send_cmd(port, cmd, extra, extra_len);
    if (err != ESP_OK) {
        goto out;
    }

    /* 3. Wait for CMD_DONE (161) */
    int64_t ack_deadline = now_us() + 5000000LL;  /* 5 s for ack */
    if (ack_deadline > deadline) {
        ack_deadline = deadline;
    }
    err = uart_scan_byte(port, AMBIT_CMD_DONE, ack_deadline);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ch%u: CMD_DONE not received", channel);
        goto out;
    }

    /* 4. Receive response */
    if (expect_raw == UART_QUERY_ACK_ONLY) {
        /* Config commands: CMD_DONE is the only response, no CMD_END */
        response->status = ESP_OK;
        err = ESP_OK;
    } else if (expect_raw > 0 && expect_raw != UART_QUERY_ACK_ONLY) {
        err = receive_raw_response(port, response, expect_raw, deadline);
    } else {
        err = receive_fsm_response(port, response, deadline);
    }

    if (err == ESP_OK) {
        s_ch[channel].state = UART_SENSOR_CONNECTED;
        s_ch[channel].ping_ok = true;
        s_ch[channel].ping_ts = now_us();
    }

out:
    if (err != ESP_OK) {
        response->status = err;
        s_ch[channel].state = UART_SENSOR_DISCONNECTED;
    }
    channel_release(channel);
    return err;
}

/* ── Public: ping ──────────────────────────────────────────────────── */

static esp_err_t do_ping(uint8_t channel, bool *connected)
{
    if (!s_inited || channel >= UART_SENSOR_NUM_CHANNELS) {
        return ESP_ERR_INVALID_STATE;
    }

    *connected = false;

    /* Check cache */
    int64_t age = now_us() - s_ch[channel].ping_ts;
    if (s_ch[channel].ping_ts > 0 && age < PING_CACHE_TTL_US) {
        *connected = s_ch[channel].ping_ok;
        return ESP_OK;
    }

    esp_err_t err = channel_acquire(channel, pdMS_TO_TICKS(PING_TIMEOUT_MS));
    if (err != ESP_OK) {
        return err;
    }

    uart_port_t port = s_ch[channel].uart_num;
    int64_t deadline = now_us() + (int64_t)PING_TIMEOUT_MS * 1000;

    err = ambit_wake(port, deadline);
    bool ok = (err == ESP_OK);

    s_ch[channel].ping_ok = ok;
    s_ch[channel].ping_ts = now_us();
    s_ch[channel].state   = ok ? UART_SENSOR_CONNECTED : UART_SENSOR_DISCONNECTED;

    *connected = ok;
    channel_release(channel);
    return ESP_OK;      /* ping itself succeeds; result is in *connected */
}

/* ── Public: status ────────────────────────────────────────────────── */

static esp_err_t do_status(uint8_t channel, uart_sensor_state_t *out)
{
    if (!s_inited || channel >= UART_SENSOR_NUM_CHANNELS) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_ch[channel].state;
    return ESP_OK;
}

/* ── Init ──────────────────────────────────────────────────────────── */

esp_err_t uart_sensors_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Channel table — pin assignments from hardware schematic */
    s_ch[0] = (channel_t){ .uart_num = UART_NUM_1, .rx_pin =  3, .tx_pin = 46, .shared = false };
    s_ch[1] = (channel_t){ .uart_num = UART_NUM_2, .rx_pin = 17, .tx_pin = 18, .shared = false };
    s_ch[2] = (channel_t){ .uart_num = UART_NUM_0, .rx_pin = 47, .tx_pin = 48, .shared = true  };
    s_ch[3] = (channel_t){ .uart_num = UART_NUM_0, .rx_pin = 40, .tx_pin = 41, .shared = true  };

    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* ── UART1 — dedicated to CH0 (AMBIT1) ── */
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_1, &uart_cfg), TAG, "UART1 cfg");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_NUM_1, s_ch[0].tx_pin, s_ch[0].rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "UART1 pins");
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_1, UART_RX_BUF_SIZE, 0, 0, NULL, 0),
                        TAG, "UART1 install");

    /* ── UART2 — dedicated to CH1 (AMBIT2) ── */
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_2, &uart_cfg), TAG, "UART2 cfg");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_NUM_2, s_ch[1].tx_pin, s_ch[1].rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "UART2 pins");
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_2, UART_RX_BUF_SIZE, 0, 0, NULL, 0),
                        TAG, "UART2 install");

    /* ── UART0 — shared between CH2 (AMBIT3) and CH3 (AMBIT4)
     *    Default mapping: CH2 pins.  Remapped on-demand. ── */
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_0, &uart_cfg), TAG, "UART0 cfg");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_NUM_0, s_ch[2].tx_pin, s_ch[2].rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "UART0 pins");
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_0, UART_RX_BUF_SIZE, 0, 0, NULL, 0),
                        TAG, "UART0 install");

    /* ── Mutexes ── */
    s_uart0_mtx = xSemaphoreCreateMutex();
    if (s_uart0_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < UART_SENSOR_NUM_CHANNELS; i++) {
        s_ch_mtx[i] = xSemaphoreCreateMutex();
        if (s_ch_mtx[i] == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_inited = true;
    ESP_LOGI(TAG, "UART sensors initialised (4 channels, Option D)");
    return ESP_OK;
}

/* ── ASCII line-oriented query (generic UART sensors) ─────────────── *
 *
 * Generic alternative to do_query() above. Sends `cmd` followed by
 * `terminator`, then accumulates incoming bytes until `terminator` is seen
 * (the terminator itself is stripped from the response) or `timeout_ms`
 * elapses. Use for sensors that speak line-oriented ASCII (SCPI, AT-style,
 * custom NMEA-ish protocols) — not for the AMBIT binary framing.
 */
static esp_err_t do_text_query(uint8_t channel,
                               const char *cmd, const char *terminator,
                               char *out_resp, size_t resp_cap, size_t *resp_len,
                               uint32_t timeout_ms)
{
    if (channel >= UART_SENSOR_NUM_CHANNELS || cmd == NULL || terminator == NULL
            || out_resp == NULL || resp_cap < 2 || resp_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    *resp_len = 0;
    out_resp[0] = '\0';

    size_t term_len = strlen(terminator);
    if (term_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t deadline = now_us() + (int64_t)timeout_ms * 1000;

    esp_err_t err = channel_acquire(channel, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        return err;
    }

    uart_port_t port = s_ch[channel].uart_num;
    uart_flush_input(port);

    /* Send: cmd + terminator, atomically inside the lock. */
    size_t cmd_len = strlen(cmd);
    if (cmd_len > 0) {
        uart_write_bytes(port, cmd, cmd_len);
    }
    uart_write_bytes(port, terminator, term_len);

    /* Read byte-by-byte until terminator suffix is hit or the deadline trips. */
    while (*resp_len + 1 < resp_cap) {
        int64_t remain_us = deadline - now_us();
        if (remain_us <= 0) break;

        TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remain_us / 1000));
        if (ticks == 0) ticks = 1;

        uint8_t b;
        int n = uart_read_bytes(port, &b, 1, ticks);
        if (n <= 0) {
            continue; /* no byte yet; loop and re-check deadline */
        }

        out_resp[(*resp_len)++] = (char)b;

        /* Suffix-match against the terminator. */
        if (*resp_len >= term_len &&
            memcmp(out_resp + *resp_len - term_len, terminator, term_len) == 0) {
            *resp_len -= term_len;
            out_resp[*resp_len] = '\0';
            s_ch[channel].state = UART_SENSOR_CONNECTED;
            channel_release(channel);
            return ESP_OK;
        }
    }

    out_resp[*resp_len] = '\0';
    s_ch[channel].state = UART_SENSOR_DISCONNECTED;
    channel_release(channel);
    return ESP_ERR_TIMEOUT;
}

/* ── Streaming ASCII query (multi-line until a sentinel) ──────────────── *
 *
 * For sensors that answer a command with many lines and a final sentinel line
 * (e.g. the AMBIT PLOTTING run: many "T:..,F:.." lines then a "Done" line).
 * Pre-wakes the port (the AMBIT light-sleeps and drops the first bytes on UART
 * wake; a lone newline is parsed as an empty command and wakes it), sends
 * cmd+terminator, then accumulates everything into `out` until a line
 * containing `sentinel` arrives or `timeout_ms` elapses. */
static esp_err_t do_stream_query(uint8_t channel,
                                 const char *cmd, const char *terminator,
                                 const char *sentinel,
                                 char *out, size_t cap, size_t *out_len,
                                 uint32_t timeout_ms)
{
    if (channel >= UART_SENSOR_NUM_CHANNELS || cmd == NULL ||
            out == NULL || cap < 2 || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_len = 0;
    out[0] = '\0';
    int64_t deadline = now_us() + (int64_t)timeout_ms * 1000;

    esp_err_t err = channel_acquire(channel, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        return err;
    }
    uart_port_t port = s_ch[channel].uart_num;

    /* Wake with the proper 3-byte handshake and WAIT for the AMBIT's ACK before
     * sending the command. The AMBIT selects ASCII vs binary command mode from
     * the first received byte (>127 => binary); if the command's first byte is
     * the one mangled by the UART wake transient and lands >127, the AMBIT
     * misroutes to binary mode and rejects the whole command ("Unknown cmd N").
     * Confirming the ack first guarantees the AMBIT is awake and the command's
     * first byte arrives clean. */
    esp_err_t wake_err = ambit_wake(port, deadline);
    if (wake_err != ESP_OK) {
        s_ch[channel].state = UART_SENSOR_DISCONNECTED;
        channel_release(channel);
        return wake_err;   /* AMBIT never acked — treat as disconnected */
    }
    uart_flush_input(port);   /* drop the ACK / boot-idle bytes */

    size_t cmd_len = strlen(cmd);
    if (cmd_len > 0) {
        uart_write_bytes(port, cmd, cmd_len);
    }
    if (terminator != NULL && terminator[0] != '\0') {
        uart_write_bytes(port, terminator, strlen(terminator));
    }

    size_t line_start = 0;
    size_t sent_len   = (sentinel != NULL) ? strlen(sentinel) : 0;
    while (*out_len + 1 < cap) {
        int64_t remain_us = deadline - now_us();
        if (remain_us <= 0) break;
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remain_us / 1000));
        if (ticks == 0) ticks = 1;

        uint8_t b;
        int n = uart_read_bytes(port, &b, 1, ticks);
        if (n <= 0) {
            continue;
        }
        out[(*out_len)++] = (char)b;
        if (b == '\n') {
            out[*out_len] = '\0';
            if (sent_len > 0 && strstr(out + line_start, sentinel) != NULL) {
                break; /* sentinel line received — done */
            }
            line_start = *out_len;
        }
    }

    out[*out_len] = '\0';
    s_ch[channel].state = (*out_len > 0) ? UART_SENSOR_CONNECTED : UART_SENSOR_DISCONNECTED;
    channel_release(channel);
    return (*out_len > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ── Port-adapter getters ──────────────────────────────────────────── */

uart_sensor_query_fn       uart_sensors_get_query_fn(void)       { return do_query;      }
uart_sensor_ping_fn        uart_sensors_get_ping_fn(void)        { return do_ping;       }
uart_sensor_status_fn      uart_sensors_get_status_fn(void)      { return do_status;     }
uart_sensor_text_query_fn  uart_sensors_get_text_query_fn(void)  { return do_text_query; }
uart_sensor_stream_query_fn uart_sensors_get_stream_query_fn(void) { return do_stream_query; }
