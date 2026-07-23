#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timezone / DST support for the RTC-based scheduler.
 *
 * The device clock and every stored/published timestamp are UTC
 * (measurement_time_utc); the cloud renders local time from the IANA name in the
 * MQTT envelope. But the ON-DEVICE scheduler (components/time_sync + sched.lua)
 * reasons in LOCAL wall time — "08:00", "30 min after sunset", the day/night
 * gate. This module bridges the two: it turns the configured IANA name into a
 * libc POSIX-TZ rule (which embeds the DST transition dates — no tzdata files
 * needed) and hands time_sync a DST-correct local-minus-UTC offset.
 *
 * ONLY scheduling depends on this. If the IANA name is unset/unknown, the offset
 * falls back to time_sync's current value (the +2 h CEST default, or a manual
 * `sync loc <lat> <lon> <tz>`), so scheduling degrades to a fixed offset instead
 * of breaking. Stored/published timestamps are never affected either way. */

/* Resolve `iana` (e.g. "Europe/Amsterdam") to a POSIX TZ string and install it
 * (setenv TZ + tzset) so libc localtime_r is DST-correct. NULL/empty/unknown
 * leaves the offset on the time_sync fallback. Call at boot and whenever the
 * timezone config changes. */
void timezone_apply(const char *iana);

/* Local-minus-UTC offset in seconds for the instant `utc`, DST-resolved from the
 * applied zone. Returns the time_sync fallback offset when no zone is applied. */
int32_t timezone_utc_offset_seconds(int64_t utc);

/* Convenience for the scheduler boundary: localize a UTC epoch for handing to
 * time_sync, refreshing time_sync's stored offset so its sunrise/sunset path
 * uses the same value. Returns utc + timezone_utc_offset_seconds(utc). */
int64_t timezone_localize(int64_t utc);

#ifdef __cplusplus
}
#endif
