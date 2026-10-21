#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void svc_usb_notice_handler(void (*handler)(const char *reason));
bool svc_usb_enter(void);
void svc_usb_leave(void);
bool svc_usb_active(void);
bool svc_usb_take_overrun(void);
void svc_usb_counters(uint32_t *calls, uint32_t *bytes);
int svc_usb_read(void *buffer, size_t length, unsigned timeout_ms);
bool svc_usb_write(const void *buffer, size_t length);
/* Column-zero notice outside service, without resetting RX/editor state. */
bool svc_usb_notice_write(const void *buffer, size_t length);
