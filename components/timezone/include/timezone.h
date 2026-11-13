#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Timezone / DST support for the RTC-based scheduler.
 *
 * The device clock and every stored/published timestamp are UTC
 * (measurement_time_utc); the cloud renders local time from the IANA name in the
 * MQTT envelope. But the ON-DEVICE scheduler (components/time_sync + sched_spec)
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
 * (setenv TZ + tzset) so libc localtime_r is DST-correct. Call at boot and
 * whenever the timezone config changes.
 *   ESP_OK                 installed and verified (or NULL/empty: unset, the
 *                          time_sync fallback offset is in charge)
 *   ESP_ERR_INVALID_ARG    unknown name: cleared to the fallback offset
 *   ESP_ERR_NO_MEM         setenv failed: the previous zone stays active
 *   ESP_ERR_INVALID_STATE  libc did not run the rule: the previous verified
 *                          zone was restored, or (none) the zone's fixed
 *                          prevailing offset is used without DST
 * Only ESP_OK may be reported as "applied". */
esp_err_t timezone_apply(const char *iana);

/* True while a verified libc rule is installed (the last apply succeeded or
 * restored one). False when unset or degraded to a fixed offset. */
bool timezone_rule_active(void);

/* ISO 3166 alpha-2 country of a supported zone name ("NL", "AU"), from
 * tzdata zone.tab; "" for unknown names and zones without a country (Etc).
 * Used to decide whether a live location estimate or the configured location
 * wins (docs/timezone-from-location.md). Table-owned, immutable. */
const char *timezone_country(const char *iana);

/* Immutable table-owned IANA name currently applied to the scheduler. Safe to
 * read concurrently with apply; valid for the lifetime of the firmware. */
const char *timezone_current_name(void);
/* One consistent name/offset pair for a publication, even if apply runs on
 * another task between constructing timestamp_local and the zone label. */
void timezone_snapshot(int64_t utc, const char **name, int32_t *offset);

/* Validate and canonicalize a configured timezone before it is persisted or
 * published. Whitespace is trimmed, the historical JII shorthand "AMT" is
 * migrated to "Europe/Amsterdam", and "Z" is canonicalized to "UTC". Only
 * zones supported by this firmware's POSIX-rule table are accepted. Empty or
 * NULL input represents an unset timezone and succeeds with an empty output.
 * Returns ESP_ERR_INVALID_ARG for an unknown timezone and
 * ESP_ERR_INVALID_SIZE when the destination is too small. */
esp_err_t timezone_canonicalize(const char *input, char *output, size_t output_size);

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
