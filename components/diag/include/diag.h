#pragma once
/*
 * diag.h — per-boot device diagnostics (remote diagnostics, 2026-11):
 * restart reason, a persisted boot counter and the stored-coredump summary,
 * computed ONCE per boot in app_main's task context and cached in RAM. Every
 * status message then only reads the cache:
 *   - ambyte.telemetry/1 (5-min stored heartbeat, the direct heartbeat and the
 *     schedule's device/status-report): health.runtime.reset_reason +
 *     boot_count, and health.crash;
 *   - pong: reset_reason + boot_count.
 * Error log lines are diag_log.h.
 */
#include <stdbool.h>
#include <stdint.h>

#include "diag_crash.h"
#include "diag_log.h"
#include "diag_reset.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, after nvs_flash_init() and before anything publishes. Bumps the
 * NVS boot counter, caches esp_reset_reason(), reads + verifies the coredump
 * partition (read-only: never erased) and persists the dump's first-seen boot.
 * Never fails boot: every error degrades to a reported detail. */
void diag_boot_init(void);

/* Cached values; safe from any task after diag_boot_init (before it:
 * "unknown" / 0 / {"present":false,"error":"not_checked"}). */
const char *diag_reset_reason(void);
/* NVS boot counter, incremented once per boot; 0 = NVS unavailable. */
uint32_t    diag_boot_count(void);
/* Pre-rendered health.crash object (always a complete JSON object). */
const char *diag_crash_json(void);
/* Random 32-bit value fixed for this boot, never 0 (review round 2, finding
 * 4). Identifies the boot even when boot_count is 0 (NVS unavailable), so a
 * remote log reader's `gen` can never repeat across reboots. Drawn on first
 * use, not in diag_boot_init: esp_random() is only fully random once the RF
 * subsystem runs, and the first user is a log_read/log_list command, which
 * arrived over Wi-Fi. */
uint32_t    diag_boot_nonce(void);

#ifdef __cplusplus
}
#endif
