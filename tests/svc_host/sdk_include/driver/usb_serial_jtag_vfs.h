#pragma once
#include "esp_vfs.h"
void usb_serial_jtag_vfs_use_driver(void);
void usb_serial_jtag_vfs_set_tx_line_endings(esp_line_endings_t);
void usb_serial_jtag_vfs_set_rx_line_endings(esp_line_endings_t);
const esp_vfs_fs_ops_t *esp_vfs_usb_serial_jtag_get_vfs(void);
