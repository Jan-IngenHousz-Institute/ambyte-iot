#ifndef AMBYTE_TIME_SYNC_H
#define AMBYTE_TIME_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RTC-based measurement scheduling math (pure, no hardware access).
 *
 * Time model: every `now`/`date_unix` argument is LOCAL Unix seconds. The RTC
 * (and every stored/published timestamp) is UTC; callers localize by ADDING the
 * configured UTC offset before calling in — see components/timezone, which
 * derives a DST-correct offset from the device's IANA timezone and pushes it
 * here via time_sync_set_utc_offset_seconds(). Clock/interval/weekday math then
 * runs on the local wall clock, and sunrise/sunset (computed in UTC from lat/lon)
 * is shifted by the SAME offset so it lines up. Returned values are always
 * durations (seconds-until), which are frame-independent — so the caller waits
 * the same number of seconds regardless of the frame it localized `now` into.
 */

enum {
    TIME_SYNC_SUNRISE = 0,
    TIME_SYNC_SUNSET  = 1,
};

/* Location / timezone. Defaults: 52.173 N, 5.819 E, tz = +2 (NL summer / CEST).
 * tz_offset_hours = whole hours the local clock is ahead of UTC (CEST=2, CET=1,
 * UTC=0). Sets both the sunrise/sunset shift AND the offset callers use to
 * localize `now`; get rounds the stored seconds to hours. */
void time_sync_set_location(double lat, double lon, int tz_offset_hours);
void time_sync_get_location(double *lat, double *lon, int *tz_offset_hours);

/* Set/get the local-minus-UTC offset in seconds — the DST-aware value normally
 * owned by components/timezone. Finer-grained than set_location()'s whole hours
 * and applied to every path (localized `now` + sunrise/sunset). */
void    time_sync_set_utc_offset_seconds(int32_t seconds);
int32_t time_sync_get_utc_offset_seconds(void);

/* Break a local Unix time into calendar fields. Any out-pointer may be NULL.
 * wday: 0=Sun .. 6=Sat. */
void time_sync_localtime(int64_t unix_local, int *year, int *month, int *day,
                         int *hour, int *min, int *sec, int *wday);

/* Compose a local Unix time from calendar fields (wall-clock, no tz math). */
int64_t time_sync_make(int year, int month, int day, int hour, int min, int sec);

/* Seconds from `now` until the next trigger (always > 0). */

/* Clock-aligned grid: period=600 → :00/:10/:20…, 1800 → :00/:30. `phase`
 * shifts the grid (e.g. period 3600, phase 300 → HH:05). */
int64_t time_sync_until_interval(int64_t now, int64_t period, int64_t phase);

/* Next HH:MM[:SS] (today or tomorrow). */
int64_t time_sync_until_clock(int64_t now, int hour, int min, int sec);

/* Next matching weekday at HH:MM. days_mask bit i set → weekday i (0=Sun..6=Sat).
 * Returns -1 if days_mask == 0. */
int64_t time_sync_until_weekly(int64_t now, uint8_t days_mask, int hour, int min);

/* Sunrise/sunset (as a LOCAL Unix time) for the date containing `date_unix`.
 * event = TIME_SYNC_SUNRISE/SUNSET. Returns ESP_OK + *out_unix, or
 * ESP_ERR_NOT_FOUND on polar day/night. */
esp_err_t time_sync_sun_on_date(int64_t date_unix, int event, int64_t *out_unix);

/* Seconds from `now` until (sun event + offset_sec). offset signed:
 * -1800 = 30 min before. Returns -1 if no event within the next 2 days. */
int64_t time_sync_until_sun(int64_t now, int event, int64_t offset_sec);

/* True when `now` is between sunrise and sunset (next sunset sooner than next
 * sunrise). Falls back to true on polar day/night. */
bool time_sync_is_daytime(int64_t now);

/* Map a weekday name ("sun".."sat", case-insensitive, first 3 chars) to its bit
 * index 0..6, or -1 if unrecognised. */
int time_sync_day_bit(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_TIME_SYNC_H */
