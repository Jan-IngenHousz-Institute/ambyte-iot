#pragma once
#include "../../include/driver/usb_serial_jtag.h"
#include "esp_err.h"
int usb_serial_jtag_write_bytes(const void *,size_t,TickType_t);
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *);
esp_err_t usb_serial_jtag_driver_uninstall(void);
bool usb_serial_jtag_is_driver_installed(void);
