#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "sensing_port.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pcf2131tfy_rtc_init(void);

bool pcf2131tfy_rtc_is_ready(void);

esp_err_t pcf2131tfy_rtc_get_time(time_t *out_time);
clock_read_fn pcf2131tfy_rtc_get_clock_read_fn(void);

/* Set the RTC from a broken-down UTC time, then immediately re-sync the system
 * clock from it. Writing the time clears the oscillator-stop flag, so this is
 * how a factory-fresh RTC is brought online. `utc_tm` is not modified. */
esp_err_t pcf2131tfy_rtc_set_time(const struct tm *utc_tm);

/* Convenience: set the RTC from a UTC epoch (seconds). Wraps set_time (gmtime_r +
 * set), so it also re-syncs the system clock. Matches clock_set_fn for injection
 * into device_commands (the MQTT set_time path). */
esp_err_t pcf2131tfy_rtc_set_epoch(time_t epoch_utc);

/* Read the RTC and push it into the ESP system clock (settimeofday) when the
 * read is valid (oscillator running). No-op leaving the system clock untouched
 * when the RTC time is invalid/unreadable (e.g. a never-set RTC). Returns the
 * read result. */
esp_err_t pcf2131tfy_rtc_sync_system_clock(void);

/* Start a periodic background re-sync of the system clock from the RTC every
 * `period_seconds`. Corrects system-clock drift over long uptimes and recovers
 * the clock without a reboot if the RTC is set/validated after boot. Idempotent;
 * period_seconds must be > 0. */
esp_err_t pcf2131tfy_rtc_start_periodic_sync(uint32_t period_seconds);

#ifdef __cplusplus
}
#endif
