/*
 * time_sync.c — RTC-based measurement scheduling math (pure C, no hardware).
 * Port of docs/sync.lua: Hinnant civil-date algorithms + the NOAA sunrise
 * equation. All times are LOCAL (RTC) Unix seconds; see time_sync.h.
 */

#include "time_sync.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#define TS_PI  3.14159265358979323846
#define TS_DEG (TS_PI / 180.0)

/* ── location / timezone config (defaults: NL, CEST) ─────────────────── */
static double s_lat = 52.173;   /* degrees north */
static double s_lon = 5.819;    /* degrees east (positive = east) */
static int    s_tz  = 2;        /* hours RTC is ahead of UTC (sun events only) */

void time_sync_set_location(double lat, double lon, int tz_offset_hours)
{
    s_lat = lat;
    s_lon = lon;
    s_tz  = tz_offset_hours;
}

void time_sync_get_location(double *lat, double *lon, int *tz_offset_hours)
{
    if (lat) *lat = s_lat;
    if (lon) *lon = s_lon;
    if (tz_offset_hours) *tz_offset_hours = s_tz;
}

/* ── calendar helpers (Howard Hinnant's civil algorithms) ────────────── *
 * Valid for the positive Unix range we use (years > 1970), where C integer
 * truncation equals floor division. */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int mp  = (m > 2) ? (m - 3) : (m + 9);
    int doy = (153 * mp + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;   /* days since 1970-01-01 */
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy  = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp  = (5 * doy + 2) / 153;
    int64_t dd  = doy - (153 * mp + 2) / 5 + 1;
    int64_t mm  = (mp < 10) ? (mp + 3) : (mp - 9);
    if (y) *y = (int)(mm <= 2 ? yy + 1 : yy);
    if (m) *m = (int)mm;
    if (d) *d = (int)dd;
}

void time_sync_localtime(int64_t u, int *year, int *month, int *day,
                         int *hour, int *min, int *sec, int *wday)
{
    int64_t days = u / 86400;
    int64_t rem  = u - days * 86400;
    civil_from_days(days, year, month, day);
    if (hour) *hour = (int)(rem / 3600);
    if (min)  *min  = (int)((rem % 3600) / 60);
    if (sec)  *sec  = (int)(rem % 60);
    if (wday) *wday = (int)(((days % 7) + 4) % 7);   /* 1970-01-01 = Thu(4); 0=Sun */
}

int64_t time_sync_make(int year, int month, int day, int hour, int min, int sec)
{
    return days_from_civil(year, month, day) * 86400
         + (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
}

/* ── schedulers ──────────────────────────────────────────────────────── */

int64_t time_sync_until_interval(int64_t now, int64_t period, int64_t phase)
{
    if (period <= 0) return -1;
    int64_t n = (now - phase) / period + 1;
    return (n * period + phase) - now;
}

int64_t time_sync_until_clock(int64_t now, int hour, int min, int sec)
{
    int y, mo, d;
    time_sync_localtime(now, &y, &mo, &d, NULL, NULL, NULL, NULL);
    int64_t target = time_sync_make(y, mo, d, hour, min, sec);
    if (target <= now) target += 86400;
    return target - now;
}

int64_t time_sync_until_weekly(int64_t now, uint8_t days_mask, int hour, int min)
{
    if (days_mask == 0) return -1;
    for (int ahead = 0; ahead <= 7; ahead++) {
        int y, mo, d, wd;
        time_sync_localtime(now + (int64_t)ahead * 86400, &y, &mo, &d, NULL, NULL, NULL, &wd);
        if (days_mask & (1u << wd)) {
            int64_t target = time_sync_make(y, mo, d, hour, min, 0);
            if (target > now) return target - now;
        }
    }
    return -1;
}

esp_err_t time_sync_sun_on_date(int64_t date_unix, int event, int64_t *out_unix)
{
    if (out_unix == NULL) return ESP_ERR_INVALID_ARG;

    int y, mo, d;
    time_sync_localtime(date_unix, &y, &mo, &d, NULL, NULL, NULL, NULL);

    double lw  = -s_lon;                  /* west longitude positive */
    double phi = s_lat;
    double Jd0 = (double)days_from_civil(y, mo, d) + 2440587.5;
    double n   = floor(Jd0 - 2451545.0 - 0.0009 - lw / 360.0 + 0.5);
    double J   = 2451545.0 + 0.0009 + lw / 360.0 + n;
    double M   = fmod(357.5291 + 0.98560028 * (J - 2451545.0), 360.0);
    if (M < 0) M += 360.0;
    double C   = 1.9148 * sin(M * TS_DEG) + 0.0200 * sin(2 * M * TS_DEG)
               + 0.0003 * sin(3 * M * TS_DEG);
    double lam = fmod(M + C + 180.0 + 102.9372, 360.0);
    if (lam < 0) lam += 360.0;
    double Jtr = J + 0.0053 * sin(M * TS_DEG) - 0.0069 * sin(2 * lam * TS_DEG);
    double dec = asin(sin(lam * TS_DEG) * sin(23.44 * TS_DEG)) / TS_DEG;
    double cosw = (sin(-0.833 * TS_DEG) - sin(phi * TS_DEG) * sin(dec * TS_DEG))
                / (cos(phi * TS_DEG) * cos(dec * TS_DEG));
    if (cosw < -1.0 || cosw > 1.0) return ESP_ERR_NOT_FOUND;   /* polar day/night */
    double w0  = acos(cosw) / TS_DEG;
    double Jev = (event == TIME_SYNC_SUNSET) ? (Jtr + w0 / 360.0) : (Jtr - w0 / 360.0);
    double unix_utc = (Jev - 2440587.5) * 86400.0;
    *out_unix = (int64_t)floor(unix_utc + (double)s_tz * 3600.0 + 0.5);  /* → local */
    return ESP_OK;
}

int64_t time_sync_until_sun(int64_t now, int event, int64_t offset_sec)
{
    for (int ahead = 0; ahead <= 2; ahead++) {
        int64_t ev = 0;
        if (time_sync_sun_on_date(now + (int64_t)ahead * 86400, event, &ev) == ESP_OK) {
            int64_t target = ev + offset_sec;
            if (target > now) return target - now;
        }
    }
    return -1;
}

bool time_sync_is_daytime(int64_t now)
{
    int64_t to_set  = time_sync_until_sun(now, TIME_SYNC_SUNSET,  0);
    int64_t to_rise = time_sync_until_sun(now, TIME_SYNC_SUNRISE, 0);
    if (to_set < 0 || to_rise < 0) {
        return true;                 /* polar day/night — treat as day */
    }
    return to_set < to_rise;         /* sunset sooner than sunrise ⇒ daytime */
}

int time_sync_day_bit(const char *name)
{
    static const char *const k[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
    if (name == NULL) return -1;
    for (int i = 0; i < 7; i++) {
        if (strncasecmp(name, k[i], 3) == 0) return i;
    }
    return -1;
}
