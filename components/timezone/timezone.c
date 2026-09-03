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

/* The IANA → POSIX-TZ table, generated from the IANA tzdata by
 * tools/gen_tz_table.py: every zone name in the database (canonical names plus
 * their historical aliases), the deduplicated POSIX rules they share, and each
 * zone's prevailing offset as a fallback. POSIX rules embed the DST
 * transition dates, so this needs no tzdata files (ESP-IDF ships none).
 *
 * It was a hand-written, Europe-only list until a Bolivian on-boarding tried
 * `cfg set timezone America/La_Paz` and got ESP_ERR_INVALID_ARG. Rather than
 * chase deployments one region at a time — every miss costs a firmware release,
 * and an unset zone silently leaves the scheduler on time_sync's +2 h CEST
 * default (6 h out in La Paz) — the whole database is now compiled in. Cost is
 * ~17 KB of rodata (598 names + 94 deduplicated rules, measured); do not
 * hand-edit the table, regenerate it. */
#include "tz_zone_table.inc"

/* Longest name in the table; also the NVS / canonicalize buffer ceiling. */
#define TZ_NAME_MAX 47

/* Whether a POSIX TZ rule is currently installed (timezone_apply succeeded). */
static bool s_applied = false;

/* Bytewise binary search over k_tz_zones (the generator emits it strcmp-sorted).
 * ~10 strcmps for 598 zones; this runs at boot and on config change only. */
static const tz_zone_t *zone_lookup(const char *iana)
{
    if (iana == NULL || iana[0] == '\0') return NULL;
    size_t lo = 0;
    size_t hi = TZ_TABLE_ZONE_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(iana, k_tz_zones[mid].iana);
        if (cmp == 0) return &k_tz_zones[mid];
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

esp_err_t timezone_canonicalize(const char *input, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return ESP_ERR_INVALID_ARG;
    output[0] = '\0';
    if (input == NULL) return ESP_OK;

    while (*input == ' ' || *input == '\t' || *input == '\r' || *input == '\n') input++;
    size_t len = strlen(input);
    while (len > 0) {
        char c = input[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        len--;
    }
    if (len == 0) return ESP_OK;

    const char *canonical = input;
    size_t canonical_len = len;
    if (len == 3 && strncmp(input, "AMT", len) == 0) {
        canonical = "Europe/Amsterdam";
        canonical_len = strlen(canonical);
    } else if (len == 1 && input[0] == 'Z') {
        canonical = "UTC";
        canonical_len = strlen(canonical);
    }

    /* Anything longer than the longest table entry cannot be a known zone (and
     * would not fit NVS either), so it is rejected before the lookup. */
    if (canonical_len > TZ_NAME_MAX) return ESP_ERR_INVALID_ARG;
    char probe[TZ_NAME_MAX + 1];
    memcpy(probe, canonical, canonical_len);
    probe[canonical_len] = '\0';
    if (zone_lookup(probe) == NULL) return ESP_ERR_INVALID_ARG;

    /* Names are kept exactly as the caller wrote them — deprecated aliases
     * ("Asia/Calcutta", "US/Pacific") are NOT rewritten to their canonical
     * target. Hosts and provisioning tools report whichever name their own
     * tzdata uses, and read-back verification (flash_gui) compares strings. */
    if (canonical_len + 1 > output_size) return ESP_ERR_INVALID_SIZE;
    memcpy(output, canonical, canonical_len);
    output[canonical_len] = '\0';
    return ESP_OK;
}

/* Local-minus-UTC seconds as libc currently sees `utc`. Robust without relying
 * on tm_gmtoff: fold the calendar fields down to seconds, using tm_yday for the
 * day delta and clamping across a year boundary to ±1. */
static int32_t libc_offset_seconds(int64_t utc)
{
    time_t t = (time_t)utc;
    struct tm lt, gt;
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    int days = lt.tm_yday - gt.tm_yday;
    if (lt.tm_year != gt.tm_year) days = (lt.tm_year > gt.tm_year) ? 1 : -1;
    return ((int32_t)(days * 24 + (lt.tm_hour - gt.tm_hour)) * 60
            + (lt.tm_min - gt.tm_min)) * 60 + (lt.tm_sec - gt.tm_sec);
}

/* Did libc actually accept the rule we just installed?
 *
 * newlib's tzset() parses the TZ string leniently and gives no error: a field it
 * cannot scan simply ends the parse, leaving localtime_r on UTC. That failure is
 * invisible — the scheduler would just run hours off, which is the exact bug
 * this component exists to prevent. So probe two instants half a year apart
 * (one per DST season, any year will do): a zone whose table offset is non-zero
 * while libc reports zero at BOTH instants was rejected. Zones that really are
 * UTC+0 are indistinguishable and need no check. */
static bool rule_took_effect(const tz_zone_t *zone)
{
    if (zone->fallback_min == 0) return true;
    static const int64_t k_probes[2] = {
        1768478400,   /* 2026-01-15 12:00 UTC */
        1784116800,   /* 2026-07-15 12:00 UTC */
    };
    for (size_t i = 0; i < sizeof(k_probes) / sizeof(k_probes[0]); i++) {
        if (libc_offset_seconds(k_probes[i]) != 0) return true;
    }
    return false;
}

void timezone_apply(const char *iana)
{
    char canonical[48];
    esp_err_t canonical_err = timezone_canonicalize(iana, canonical, sizeof(canonical));
    if (canonical_err != ESP_OK) {
        s_applied = false;
        ESP_LOGE(TAG, "rejecting invalid timezone '%s'", iana != NULL ? iana : "(null)");
        return;
    }
    const tz_zone_t *zone = zone_lookup(canonical);
    if (zone == NULL) {
        s_applied = false;
        ESP_LOGI(TAG, "no timezone configured — scheduler uses the fallback offset");
        return;
    }
    const char *posix = k_tz_rules[zone->rule];
    setenv("TZ", posix, 1);
    tzset();
    s_applied = true;
    if (!rule_took_effect(zone)) {
        /* Keep scheduling on the zone's own standard offset rather than the
         * unrelated time_sync default: wrong by at most the DST hour, not by
         * the whole distance from Amsterdam. */
        s_applied = false;
        time_sync_set_utc_offset_seconds((int32_t)zone->fallback_min * 60);
        ESP_LOGE(TAG, "libc rejected TZ=%s for '%s' — scheduling on the fixed "
                      "offset %d min, DST will NOT be tracked",
                 posix, canonical, (int)zone->fallback_min);
        return;
    }
    ESP_LOGI(TAG, "timezone '%s' -> TZ=%s (DST-aware scheduling, tzdata %s)",
             canonical, posix, TZ_TABLE_TZDATA_VERSION);
}

int32_t timezone_utc_offset_seconds(int64_t utc)
{
    if (!s_applied) {
        return time_sync_get_utc_offset_seconds();   /* fallback: fixed offset */
    }
    return libc_offset_seconds(utc);
}

int64_t timezone_localize(int64_t utc)
{
    int32_t off = timezone_utc_offset_seconds(utc);
    time_sync_set_utc_offset_seconds(off);   /* keep the sun path on the same frame */
    return utc + off;
}
