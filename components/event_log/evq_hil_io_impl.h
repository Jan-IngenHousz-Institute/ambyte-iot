#pragma once

/* Internal to event_log (CONFIG_AMBYTE_EVQ_HIL): wrapper/fault entry points. */
#include <stdio.h>

void evq_hil_io_init(void);
void evq_hil_fault_point(const char *name);
void evq_hil_rom_reset(const char *why) __attribute__((noreturn));
