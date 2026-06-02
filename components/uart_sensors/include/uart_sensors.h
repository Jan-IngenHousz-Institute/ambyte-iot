#ifndef AMBYTE_UART_SENSORS_H
#define AMBYTE_UART_SENSORS_H

#include "esp_err.h"
#include "uart_sensor_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise all UART controllers and pin mappings for the 3 sensor channels.
 *
 * Channel layout (Option D — GPIO-remap for channels 1 & 2). The former CH0 on
 * UART1 was dropped on the USB-host branch; UART1 is now the debug console.
 *   CH0 (AMBIT2): UART2  — dedicated
 *   CH1 (AMBIT3): UART0  — shared, GPIO-remapped per query
 *   CH2 (AMBIT4): UART0  — shared, GPIO-remapped per query
 *
 * Must be called once from app_main before any query/ping.
 */
esp_err_t uart_sensors_init(void);

/* ── AMBIT ROM-flash reset/boot control (host-driven C3 firmware update) ──
 * Force an AMBIT (ESP8685/ESP32-C3) into its ROM serial download mode over the
 * shared FFC/UART so the ambyte can flash it via the ROM bootloader. Reset
 * (CHIP_EN) is shared across all channels; the boot strap (GPIO9) is per
 * channel. Lines are open-drain against the AMBIT's pull-ups. Only use inside a
 * quiesced context (Lua stopped) — a download-entry pulses the shared reset, so
 * all four AMBITs reboot (only the target enters download mode). Sequence:
 *   flash_session_begin(ch, wait) → enter_download(ch) → <flash over UART0>
 *   → run_app(ch) → flash_session_end(ch).
 * begin() takes the shared bus + remaps UART0 to `ch`; hold it across the whole
 * session. Returns ESP_ERR_TIMEOUT if the bus is busy, ESP_ERR_INVALID_STATE if
 * uart_sensors_init() has not run. */
esp_err_t uart_sensors_flash_session_begin(uint8_t ch, uint32_t wait_ms);
void      uart_sensors_flash_session_end(uint8_t ch);
esp_err_t uart_sensors_enter_download(uint8_t ch);   /* session must be held */
esp_err_t uart_sensors_run_app(uint8_t ch);          /* session must be held */

/* Port-adapter getters — return function pointers wired into device_commands */
uart_sensor_query_fn       uart_sensors_get_query_fn(void);
uart_sensor_ping_fn        uart_sensors_get_ping_fn(void);
uart_sensor_status_fn      uart_sensors_get_status_fn(void);
uart_sensor_text_query_fn  uart_sensors_get_text_query_fn(void);
uart_sensor_stream_query_fn uart_sensors_get_stream_query_fn(void);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_UART_SENSORS_H */
