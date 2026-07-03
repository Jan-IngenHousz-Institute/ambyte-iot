/*
 * loader_port_ambyte.c — esp-serial-flasher (v2) user-defined port for the ambyte.
 *
 * Byte I/O reuses uart_sensors' already-open UART0 (remapped to the target channel
 * by uart_sensors_flash_session_begin() before esp_loader_connect()); reset/boot go
 * through the uart_sensors sequencer. This avoids installing a second UART driver on
 * UART0 (which the library's bundled esp32 port would do).
 */

#include "ambit_flash_port.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "uart_sensors.h"

/* uart_sensors owns UART0 for all 4 AMBIT channels; the active channel's pins are
 * already routed onto it by uart_sensors_flash_session_begin(). */
#define FLASH_UART  UART_NUM_0

static esp_loader_error_t port_write(esp_loader_port_t *port, const uint8_t *data,
                                     uint16_t size, uint32_t timeout)
{
    (void)port;
    uart_write_bytes(FLASH_UART, (const char *)data, size);
    esp_err_t err = uart_wait_tx_done(FLASH_UART, pdMS_TO_TICKS(timeout));
    if (err == ESP_OK)          return ESP_LOADER_SUCCESS;
    if (err == ESP_ERR_TIMEOUT) return ESP_LOADER_ERROR_TIMEOUT;
    return ESP_LOADER_ERROR_FAIL;
}

static esp_loader_error_t port_read(esp_loader_port_t *port, uint8_t *data,
                                    uint16_t size, uint32_t timeout)
{
    (void)port;
    int read = uart_read_bytes(FLASH_UART, data, size, pdMS_TO_TICKS(timeout));
    if (read < 0)            return ESP_LOADER_ERROR_FAIL;
    if (read < (int)size)    return ESP_LOADER_ERROR_TIMEOUT;
    return ESP_LOADER_SUCCESS;
}

static void port_enter_bootloader(esp_loader_port_t *port)
{
    ambit_flash_port_t *p = container_of(port, ambit_flash_port_t, base);
    uart_sensors_enter_download(p->channel);   /* boot low + shared CHIP_EN pulse */
}

static void port_reset_target(esp_loader_port_t *port)
{
    ambit_flash_port_t *p = container_of(port, ambit_flash_port_t, base);
    uart_sensors_run_app(p->channel);          /* straps released + reset → app boot */
}

static void port_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    ambit_flash_port_t *p = container_of(port, ambit_flash_port_t, base);
    p->time_end_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

static uint32_t port_remaining_time(esp_loader_port_t *port)
{
    ambit_flash_port_t *p = container_of(port, ambit_flash_port_t, base);
    int64_t remaining = (p->time_end_us - esp_timer_get_time()) / 1000;
    return (remaining > 0) ? (uint32_t)remaining : 0;
}

static void port_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    if (ms == 0) return;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);   /* never round a nonzero delay down to 0 */
}

static esp_loader_error_t port_change_rate(esp_loader_port_t *port, uint32_t rate)
{
    (void)port;
    return (uart_set_baudrate(FLASH_UART, rate) == ESP_OK)
               ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

const esp_loader_port_ops_t ambit_flash_port_ops = {
    .init                     = NULL,   /* UART0 already up (uart_sensors) */
    .deinit                   = NULL,
    .enter_bootloader         = port_enter_bootloader,
    .reset_target             = port_reset_target,
    .start_timer              = port_start_timer,
    .remaining_time           = port_remaining_time,
    .delay_ms                 = port_delay_ms,
    .log                      = NULL,   /* suppress library logging */
    .log_hex                  = NULL,
    .change_transmission_rate = port_change_rate,
    .write                    = port_write,
    .read                     = port_read,
    .spi_set_cs               = NULL,
    .sdio_write               = NULL,
    .sdio_read                = NULL,
    .sdio_card_init           = NULL,
};
