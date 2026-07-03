#pragma once

/*
 * Private: the ambyte's user-defined esp-serial-flasher port (v2 vtable API).
 * Reuses uart_sensors' already-open, channel-multiplexed UART0 for byte I/O and
 * uart_sensors_enter_download()/run_app() for the reset/boot sequence, instead of
 * the library's bundled esp32 port (which would install a conflicting UART driver).
 */

#include <stdint.h>
#include "esp_loader_io.h"   /* esp_loader_port_t, esp_loader_port_ops_t, container_of */

typedef struct {
    esp_loader_port_t base;        /* embedded handle — recovered via container_of */
    uint8_t           channel;     /* AMBIT UART channel (0..3) being flashed */
    int64_t           time_end_us; /* one-shot timer deadline (esp_timer units) */
} ambit_flash_port_t;

/* The vtable, defined in loader_port_ambyte.c. */
extern const esp_loader_port_ops_t ambit_flash_port_ops;
