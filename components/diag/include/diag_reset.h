#pragma once
/*
 * diag_reset.h — the ONE reset-reason table (remote diagnostics, 2026-11).
 *
 * Before this, the only place a restart cause was ever spelled out was the
 * "boot sync: last reset was <x>" line in /sdcard/logs/ambyte.log, written by
 * ambit_flash.c's private table — so on 14 Nov 2026, when the Uluru gateways
 * restarted at the every-10-minute MPF slots and went silent, the cloud could only guess
 * the cause from timing and power readings. Every status message now carries
 * `reset_reason` from this table, and ambit_flash's log line uses it too, so
 * there is exactly one spelling of each reason anywhere (no divergent tables).
 *
 * The strings are a STABLE WIRE ENUM (health.runtime.reset_reason, pong): add
 * new values when IDF grows its enum, never rename one. Anything unmapped
 * (including ESP_RST_UNKNOWN and future IDF additions) is "unknown".
 */
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lowercase stable name for an esp_reset_reason_t: poweron, brownout, panic,
 * int_wdt, task_wdt, other_wdt, sw, deepsleep, ext, sdio, usb, jtag, efuse,
 * pwr_glitch, cpu_lockup, unknown. Never NULL. Pure (host-tested). */
const char *diag_reset_reason_name(esp_reset_reason_t reason);

#ifdef __cplusplus
}
#endif
