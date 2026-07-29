#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared with the publish clock gate and the external set-time validation. */
#define CLOCK_TRUST_VALID_FLOOR_S 1704067200LL /* 2024-01-01T00:00:00Z */

/* Apply the persisted high-water mark as a boot-time lower bound. Call once,
 * after RTC/flash_time bootstrap and before measurements can start. `rtc_suspect`
 * is sticky boot evidence that OSF/read readiness disagreed.
 * When `out_adopted` is true, `out_floor` is the epoch installed in the system
 * clock; the composition root should also write that floor to the RTC so a
 * later periodic RTC sync cannot roll the clock back. */
esp_err_t clock_trust_boot_guard(bool rtc_suspect, bool *out_adopted,
                                 time_t *out_floor);

/* Persist the current valid system epoch, monotonically. Intended for the
 * watchdog task's hourly cadence. Invalid/pre-2024 clocks are ignored. */
esp_err_t clock_trust_refresh_hwm(void);

/* Same operation for a known-good epoch (the accepted SNTP sample). */
esp_err_t clock_trust_refresh_hwm_at(time_t epoch_utc);

/* Record that the running system clock was accepted from SNTP. This changes
 * clock_src but deliberately does not clear sticky boot suspicion. */
void clock_trust_note_sntp(void);

/* Record an explicit operator/MQTT RTC set as the current source. Periodic RTC
 * drift correction does not call this, so an SNTP-derived clock remains sntp. */
void clock_trust_note_rtc(void);

/* Cheap, allocation-free STATUS snapshot. `out_source` points at immutable
 * storage and is always one of "rtc", "hwm", or "sntp". */
void clock_trust_get_status(const char **out_source, bool *out_suspect);

#ifdef __cplusplus
}
#endif
