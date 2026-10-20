#pragma once
#include <stdint.h>
#include <stdbool.h>
#define USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY 1
#define USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT 2
#define USB_SERIAL_JTAG_LL_EXT_PHY_SUPPORTED 0
uint32_t usb_serial_jtag_ll_get_intsts_mask(void);
void usb_serial_jtag_ll_clr_intsts_mask(uint32_t);
int usb_serial_jtag_ll_txfifo_writable(void);
uint32_t usb_serial_jtag_ll_read_rxfifo(void *, uint32_t);
uint32_t usb_serial_jtag_ll_write_txfifo(const void *, uint32_t);
void usb_serial_jtag_ll_txfifo_flush(void);
void usb_serial_jtag_ll_ena_intr_mask(uint32_t);
void usb_serial_jtag_ll_disable_intr_mask(uint32_t);
void usb_serial_jtag_ll_enable_bus_clock(bool);
void usb_serial_jtag_ll_phy_set_defaults(void);
