#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"
typedef int esp_err_t;
#define ESP_OK 0
typedef struct { int rx_buffer_size, tx_buffer_size; } usb_serial_jtag_driver_config_t;
int usb_serial_jtag_read_bytes(void *data, uint32_t length, TickType_t wait);
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t wait);
