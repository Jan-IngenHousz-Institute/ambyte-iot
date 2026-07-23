/*
 * timezone.c — IANA name → libc POSIX-TZ rule → DST-correct offset for the
 * on-device scheduler. See timezone.h for the model; components/time_sync
 * consumes the offset. This is the only place that touches libc TZ state, so
 * time_sync stays pure and testable.
 */

#include "timezone.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "time_sync.h"

#define TAG "timezone"

/* Whether a POSIX TZ rule is currently installed (timezone_apply succeeded). */
static bool s_applied = false;

/* Minimal IANA → POSIX-TZ map. POSIX rules embed the DST transition dates, so
 * this needs no tzdata files (ESP-IDF ships none). Covers the European zones a
 * deployment is likely to use; most of continental Europe shares one rule.
 * Extend this table as new regions are deployed — an unknown name falls back to
 * the fixed offset (see timezone.h), it does not break scheduling. */
static const struct { const char *iana; const char *posix; } k_zones[] = {
    /* Central European Time (UTC+1, DST +2) */
    { "Europe/Amsterdam",  "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Brussels",   "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris",      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vienna",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Stockholm",  "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Oslo",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Prague",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Budapest",   "CET-1CEST,M3.5.0,M10.5.0/3" },
    /* Western European Time (UTC+0, DST +1) */
    { "Europe/London",     "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Dublin",     "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Lisbon",     "WET0WEST,M3.5.0/1,M10.5.0" },
    /* Eastern European Time (UTC+2, DST +3) */
    { "Europe/Helsinki",   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Athens",     "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Bucharest",  "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    /* No DST */
    { "UTC",               "UTC0" },
    { "Etc/UTC",           "UTC0" },
};

static const char *posix_for(const char *iana)
{
    if (iana == NULL || iana[0] == '\0') return NULL;
    for (size_t i = 0; i < sizeof(k_zones) / sizeof(k_zones[0]); i++) {
        if (strcmp(iana, k_zones[i].iana) == 0) return k_zones[i].posix;
    }
    return NULL;
}

void timezone_apply(const char *iana)
{
    const char *posix = posix_for(iana);
    if (posix == NULL) {
        s_applied = false;
        if (iana != NULL && iana[0] != '\0') {
            ESP_LOGW(TAG, "no POSIX rule for timezone '%s' — scheduler uses the "
                          "fixed fallback offset (add it to k_zones for DST)", iana);
        } else {
            ESP_LOGI(TAG, "no timezone configured — scheduler uses the fallback offset");
        }
        return;
    }
    setenv("TZ", posix, 1);
    tzset();
    s_applied = true;
    ESP_LOGI(TAG, "timezone '%s' -> TZ=%s (DST-aware scheduling)", iana, posix);
}

int32_t timezone_utc_offset_seconds(int64_t utc)
{
    if (!s_applied) {
        return time_sync_get_utc_offset_seconds();   /* fallback: fixed offset */
    }
    time_t t = (time_t)utc;
    struct tm lt, gt;
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    /* localtime - gmtime as a signed second count. Robust without relying on
     * tm_gmtoff: fold the calendar fields down to seconds, using tm_yday for the
     * day delta and clamping across a year boundary to ±1. */
    int days = lt.tm_yday - gt.tm_yday;
    if (lt.tm_year != gt.tm_year) days = (lt.tm_year > gt.tm_year) ? 1 : -1;
    int32_t secs = ((int32_t)(days * 24 + (lt.tm_hour - gt.tm_hour)) * 60
                    + (lt.tm_min - gt.tm_min)) * 60 + (lt.tm_sec - gt.tm_sec);
    return secs;
}

int64_t timezone_localize(int64_t utc)
{
    int32_t off = timezone_utc_offset_seconds(utc);
    time_sync_set_utc_offset_seconds(off);   /* keep the sun path on the same frame */
    return utc + off;
}
