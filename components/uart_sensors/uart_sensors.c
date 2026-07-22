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
#include "freertos/task.h"

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
/* do_text_query prime: ms to wait after the throwaway newline for the sensor to
 * answer (and us to discard it) before sending the real command. */
#define TEXT_QUERY_PRIME_MS 50
#define WAKE_RETRIES        25
#define WAKE_RETRY_MS       50
#define PING_TIMEOUT_MS     2000
#define PING_CACHE_TTL_US   (10LL * 1000 * 1000)  /* 10 s — present-sensor re-confirm */
/* A FAILED ping is cached far longer than a successful one: an absent/empty
 * channel would otherwise be re-woken (a ~1.25 s wake-retry burst) on every SS
 * round. A present sensor still re-confirms at the short TTL above. */
#define PING_FAIL_CACHE_TTL_US (5LL * 60 * 1000 * 1000)  /* 5 min — empty channel */

/* ── AMBIT ROM-flash reset/boot control ─────────────────────────────────
 * Host-driven C3 firmware update: force an AMBIT (ESP8685/ESP32-C3) into its
 * ROM serial download mode by holding the C3 boot strap (GPIO9, per channel —
 * s_ch[].boot_pin) LOW across a CHIP_EN reset (shared, below). Both are driven
 * OPEN-DRAIN against 10k pull-ups on the AMBIT; CHIP_EN also has a 2.2uF cap
 * (RC ~22 ms), so EN must be held low long enough to discharge it and the
 * strap kept low across the full EN rise. */
#define AMBIT_RESET_GPIO        1     /* shared CHIP_EN, active-low, open-drain */
#define AMBIT_EN_LOW_HOLD_MS    20    /* hold CHIP_EN low to discharge the 2.2uF cap */
#define AMBIT_BOOT_HOLD_MS     100    /* keep GPIO9 low across the ~22 ms EN rise + margin */

/* ── Per-channel descriptor ────────────────────────────────────────── */

typedef struct {
    uart_port_t         uart_num;
    int                 rx_pin;
    int                 tx_pin;
    int                 boot_pin;    /* C3 GPIO9 boot strap (open-drain), for ROM flashing */
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

/* ── AMBIT ROM-flash: reset/boot sequencer ──────────────────────────────
 * See the AMBIT_RESET_GPIO block above. Forces one AMBIT into the ESP32-C3
 * ROM serial download mode over the shared FFC/UART so the ambyte can flash it
 * via the ROM bootloader. The reset line is SHARED, so a download-entry briefly
 * resets all four AMBITs; only the target (its boot strap held low) enters
 * download mode — the others reboot into their application. Only call inside the
 * quiesced flash context (Lua stopped), where no other channel transaction is
 * in flight. */

/* Configure CHIP_EN (shared) + the four per-channel boot straps as open-drain,
 * idle = released (Hi-Z, pulled high by the AMBIT's 10k). */
static esp_err_t ambit_boot_gpio_init(void)
{
    uint64_t mask = (1ULL << AMBIT_RESET_GPIO);
    for (int i = 0; i < UART_SENSOR_NUM_CHANNELS; i++) {
        mask |= (1ULL << s_ch[i].boot_pin);
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT_OD,   /* low = assert, released = Hi-Z */
        .pull_up_en   = GPIO_PULLUP_DISABLE,   /* external 10k pull-ups on the AMBIT */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "boot/reset gpio_config");
    /* All boot straps released first → the AMBITs boot their application, NOT ROM
     * download mode, across the reset pulse below. */
    for (int i = 0; i < UART_SENSOR_NUM_CHANNELS; i++) {
        gpio_set_level(s_ch[i].boot_pin, 1);   /* released */
    }

    /* Deliberate CHIP_EN reset pulse at init. On a normal S3 boot / OTA-restart
     * the shared reset line was previously only *released* (Hi-Z), never pulsed
     * low — so a C3 left mid-measurement by an abrupt esp_restart() (OTA, script
     * update, watchdog, CLI reboot) keeps running, and can stay hung or leave the
     * actinic LEDs energized. Pulsing EN low here, at the quiesced boot moment
     * before any measurement starts, forces all four C3s into a known clean reboot
     * with LEDs off, independent of the C3 firmware state — this is what makes a
     * "reboot mid-measurement" fully benign. Mirrors uart_sensors_run_app()'s
     * low-hold; briefly reboots all four C3s (shared line), which is fine at init. */
    gpio_set_level(AMBIT_RESET_GPIO, 0);                 /* assert reset  */
    vTaskDelay(pdMS_TO_TICKS(AMBIT_EN_LOW_HOLD_MS));     /* discharge the 2.2uF cap */
    gpio_set_level(AMBIT_RESET_GPIO, 1);                 /* release; RC ramp back up */
    return ESP_OK;
}

esp_err_t uart_sensors_flash_session_begin(uint8_t ch, uint32_t wait_ms)
{
    if (!s_inited)                      return ESP_ERR_INVALID_STATE;
    if (ch >= UART_SENSOR_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;
    /* Owns the shared bus + remaps UART0 to `ch` for the whole flash session. */
    return channel_acquire(ch, pdMS_TO_TICKS(wait_ms));
}

void uart_sensors_flash_session_end(uint8_t ch)
{
    if (ch >= UART_SENSOR_NUM_CHANNELS) return;
    channel_release(ch);   /* leaves the channel DISCONNECTED → re-pinged later */
}

esp_err_t uart_sensors_enter_download(uint8_t ch)
{
    if (ch >= UART_SENSOR_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;
    /* Only the target straps to download; the other AMBITs reboot to app. */
    for (int i = 0; i < UART_SENSOR_NUM_CHANNELS; i++) {
        gpio_set_level(s_ch[i].boot_pin, (i == ch) ? 0 : 1);
    }
    gpio_set_level(AMBIT_RESET_GPIO, 0);                 /* assert reset  */
    vTaskDelay(pdMS_TO_TICKS(AMBIT_EN_LOW_HOLD_MS));     /* discharge cap */
    gpio_set_level(AMBIT_RESET_GPIO, 1);                 /* release; RC ramp */
    vTaskDelay(pdMS_TO_TICKS(AMBIT_BOOT_HOLD_MS));       /* hold strap across the rise */
    gpio_set_level(s_ch[ch].boot_pin, 1);               /* release strap */
    uart_flush_input(UART_NUM_0);                        /* drop reset-time line noise */
    ESP_LOGI(TAG, "ch%u: AMBIT forced into ROM serial download mode", ch);
    return ESP_OK;
}

esp_err_t uart_sensors_run_app(uint8_t ch)
{
    if (ch >= UART_SENSOR_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < UART_SENSOR_NUM_CHANNELS; i++) {
        gpio_set_level(s_ch[i].boot_pin, 1);   /* all straps released → normal boot */
    }
    gpio_set_level(AMBIT_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(AMBIT_EN_LOW_HOLD_MS));
    gpio_set_level(AMBIT_RESET_GPIO, 1);
    ESP_LOGI(TAG, "ch%u: AMBIT reset to run application", ch);
    return ESP_OK;
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

/* ── Receive one FSM data array (v2: 0xD4 header marker already consumed) ──
 * The single wake + the 0xD4 header-start byte are consumed by
 * receive_fsm_response. We read the rest of the 8-byte length header
 *   {212, 150, idx, lenHi, lenLo, elem_width, dtype, csum}   csum = Σ bytes[0..6]
 * send one 200 (ready), stream `len * elem_width` data bytes + the 4-byte
 * trailer {212,0,0,csum}, verify the data byte-sum, ack 180, then widen each
 * little-endian element into the uint32 array the rest of the pipeline expects.
 * elem_width/dtype of 0 ⇒ 4/unsigned, so a v1 ambit still decodes. dtype is
 * informational: the consumer reinterprets per array index (ENV → int16). No
 * per-array wake and no per-array 210 — the run's single wake owns the ack. */
static esp_err_t fsm_receive_one_array(uart_port_t port,
                                       uart_data_array_t *out,
                                       int64_t deadline)
{
    int64_t hdr_deadline = now_us() + 2000000LL;   /* 2 s for the header tail */
    if (hdr_deadline > deadline) {
        hdr_deadline = deadline;
    }

    uint8_t hdr[8];
    hdr[0] = FSM_DATA_HDR;                          /* consumed by the caller */
    esp_err_t err = uart_read_exact(port, hdr + 1, 7, hdr_deadline);
    if (err != ESP_OK) {
        return err;
    }
    if (hdr[1] != FSM_LEN_MARKER) {
        return ESP_ERR_NOT_FOUND;                  /* stray 0xD4 — caller rescans */
    }

    uint8_t  arr_idx    = hdr[2];
    uint16_t arr_len    = ((uint16_t)hdr[3] << 8) | hdr[4];
    uint8_t  elem_width = hdr[5] ? hdr[5] : 4;     /* 0 ⇒ v1 default */

    uint8_t cs = 0;
    for (int i = 0; i < 7; i++) {
        cs += hdr[i];
    }
    if (cs != hdr[7]) {
        ESP_LOGW(TAG, "FSM header checksum mismatch (got 0x%02X, want 0x%02X)", hdr[7], cs);
        uart_write_byte(port, FSM_RESET);
        return ESP_ERR_INVALID_CRC;
    }
    if (elem_width != 2 && elem_width != 4) {
        ESP_LOGW(TAG, "FSM array %u bad elem_width %u", arr_idx, elem_width);
        uart_write_byte(port, FSM_RESET);
        return ESP_ERR_INVALID_CRC;
    }

    uart_write_byte(port, FSM_READY);              /* 200 — ready for data (once) */

    if (arr_len == 0) {
        out->index  = arr_idx;
        out->data   = NULL;
        out->length = 0;
        uart_write_byte(port, FSM_DATA_PASS);
        return ESP_OK;
    }

    size_t    nbytes = (size_t)arr_len * elem_width;
    uint8_t  *raw    = malloc(nbytes);
    uint32_t *data   = malloc((size_t)arr_len * sizeof(uint32_t));
    if (raw == NULL || data == NULL) {
        free(raw);
        free(data);
        return ESP_ERR_NO_MEM;
    }

    /* Data phase, with resend on mismatch (ambit waits for a 200 to resend). */
    for (int attempt = 0; attempt < 4; attempt++) {
        int64_t data_deadline = now_us() + 10000000LL;  /* 10 s per attempt */
        if (data_deadline > deadline) {
            data_deadline = deadline;
        }

        if (attempt > 0) {
            uart_write_byte(port, FSM_READY);           /* 200 = resend */
        }

        err = uart_read_exact(port, raw, nbytes, data_deadline);
        if (err != ESP_OK) {
            free(raw); free(data);
            return err;
        }

        uint8_t trailer[4];                             /* {212, 0, 0, csum} */
        err = uart_read_exact(port, trailer, 4, data_deadline);
        if (err != ESP_OK) {
            free(raw); free(data);
            return err;
        }

        uint8_t dcs = 0;                                /* byte-sum of the data bytes */
        for (size_t i = 0; i < nbytes; i++) {
            dcs += raw[i];
        }
        if (dcs == trailer[3]) {
            uart_write_byte(port, FSM_DATA_PASS);       /* 180 */
            for (uint16_t i = 0; i < arr_len; i++) {    /* widen LE → uint32 */
                uint32_t v = 0;
                for (uint8_t k = 0; k < elem_width; k++) {
                    v |= (uint32_t)raw[(size_t)i * elem_width + k] << (8 * k);
                }
                data[i] = v;
            }
            free(raw);
            out->index  = arr_idx;
            out->data   = data;
            out->length = arr_len;
            return ESP_OK;
        }
        ESP_LOGW(TAG, "FSM data checksum mismatch for array %u (attempt %d)",
                 arr_idx, attempt + 1);
    }

    free(raw); free(data);
    uart_write_byte(port, FSM_RESET);                   /* 222 = give up on this array */
    return ESP_ERR_INVALID_CRC;
}

/* ── Receive full FSM response (v2: one wake, then arrays until CMD_END) ──
 * After CMD_DONE the ambit runs, then wakes the host once (0xD3 → we ack 0xD2
 * exactly once) and streams every result array back-to-back as
 * {header}{data}{trailer}, finishing with 0xF0 (CMD_END). We must NOT send a
 * 210 per array — the single wake owns the ack; each array gets one 200 (ready)
 * and one 180 (pass). */
static esp_err_t receive_fsm_response(uart_port_t port,
                                      uart_sensor_response_t *resp,
                                      int64_t deadline)
{
    /* Step 1: single wake handshake. Ack the first 0xD3 with one 0xD2. A run
     * that produced nothing jumps straight to CMD_END. */
    bool awake = false;
    while (!deadline_reached(deadline)) {
        uint8_t b;
        if (uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(500)) != 1) {
            continue;
        }
        if (b == FSM_WAKE) {
            uart_write_byte(port, FSM_AWAKE);            /* 210 — ack, exactly once */
            awake = true;
            break;
        }
        if (b == AMBIT_CMD_END) {
            resp->status = ESP_OK;
            return ESP_OK;
        }
        /* skip stray bytes (debug / boot-idle) */
    }
    if (!awake) {
        resp->status = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }

    /* Step 2: arrays until CMD_END. Each starts with the 0xD4 header marker;
     * leftover 0xD3 wake bytes (queued before our ack landed) are skipped. */
    while (resp->array_count < UART_SENSOR_MAX_ARRAYS && !deadline_reached(deadline)) {
        uint8_t b;
        if (uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(500)) != 1) {
            continue;
        }
        if (b == AMBIT_CMD_END) {
            resp->status = ESP_OK;
            return ESP_OK;
        }
        if (b != FSM_DATA_HDR) {
            continue;                                    /* 0xD3 leftover / debug */
        }

        esp_err_t err = fsm_receive_one_array(
            port, &resp->arrays[resp->array_count], deadline);
        if (err == ESP_OK) {
            resp->array_count++;
        } else if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "FSM array error %s, continuing", esp_err_to_name(err));
            /* checksum: ambit resends; stray header: keep scanning */
        } else {
            resp->status = err;
            return err;
        }
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

    /* Check cache. Empty channels use the long fail-TTL so they aren't re-probed
     * every SS round; a present sensor re-confirms at the short TTL. */
    int64_t age = now_us() - s_ch[channel].ping_ts;
    int64_t ttl = s_ch[channel].ping_ok ? PING_CACHE_TTL_US : PING_FAIL_CACHE_TTL_US;
    if (s_ch[channel].ping_ts > 0 && age < ttl) {
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

    /* Channel table — pin assignments from hardware schematic.
     * Single shared bus: all four channels are time-multiplexed onto ONE UART
     * controller (UART_NUM_0), pin-remapped per transaction. The parallel
     * trigger/poll/fetch protocol keeps every host↔ambit transaction short, so
     * one controller suffices; the long measurement runs autonomously on each
     * C3. (Console is USB-Serial-JTAG, so UART0 is free for sensors.) */
    /* .boot_pin = the C3 GPIO9 strap for the AMBIT on this channel's UART pins.
     * NOTE: bound to the UART pin pair, NOT the schematic ch-label — the label
     * order differs from this s_ch[] index (see docs/ambit-uart-flash-plan.md). */
    s_ch[0] = (channel_t){ .uart_num = UART_NUM_0, .rx_pin =  3, .tx_pin = 46, .boot_pin = 2, .shared = true };
    s_ch[1] = (channel_t){ .uart_num = UART_NUM_0, .rx_pin = 17, .tx_pin = 18, .boot_pin = 7, .shared = true };
    s_ch[2] = (channel_t){ .uart_num = UART_NUM_0, .rx_pin = 47, .tx_pin = 48, .boot_pin = 6, .shared = true };
    s_ch[3] = (channel_t){ .uart_num = UART_NUM_0, .rx_pin = 40, .tx_pin = 41, .boot_pin = 5, .shared = true };

    /* AMBIT reset/boot straps (open-drain) for host-driven C3 ROM flashing. */
    ESP_RETURN_ON_ERROR(ambit_boot_gpio_init(), TAG, "AMBIT boot/reset GPIO");

    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_XTAL,   /* XTAL, not APB: keeps the 115200 baud divisor
                                         * stable under DFS / light sleep (Phase 2) so
                                         * AMBIT framing isn't corrupted by APB scaling. */
    };

    /* ── UART0 — single shared bus for all 4 channels (AMBIT1–4)
     *    Default mapping: CH0 pins. Remapped on-demand per transaction by
     *    channel_acquire(). UART1/UART2 are left free. ── */
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_0, &uart_cfg), TAG, "UART0 cfg");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_NUM_0, s_ch[0].tx_pin, s_ch[0].rx_pin,
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
    ESP_LOGI(TAG, "UART sensors initialised (4 channels, single shared UART0 bus)");
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

    /* Prime: send a bare terminator first to flush any partial line stuck in the
     * SENSOR's input buffer — e.g. leftover boot-time AMBIT wake bytes (0xAA)
     * sent by the boot auto-ping on this channel. Give the sensor a moment to
     * process + reply, then discard that reply. Without this, the leftover
     * fragment is prepended to the first real command and the sensor answers
     * "unknown command". */
    uart_flush_input(port);
    uart_write_bytes(port, terminator, term_len);
    vTaskDelay(pdMS_TO_TICKS(TEXT_QUERY_PRIME_MS));
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
