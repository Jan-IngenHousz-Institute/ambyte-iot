/*
 * timezone.c — IANA name → libc POSIX-TZ rule → DST-correct offset for the
 * on-device scheduler. See timezone.h for the model; components/time_sync
 * consumes the offset. This is the only place that touches libc TZ state, so
 * time_sync stays pure and testable.
 */

#include "timezone.h"

#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
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
/* Runtime timezone changes serialize libc TZ mutation with offset reads. Never
 * publish a mutable char buffer: telemetry takes one immutable table pointer. */
static pthread_mutex_t s_zone_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(const char *) s_zone_name = "";

static void zone_lock(void)
{
    if (pthread_mutex_lock(&s_zone_lock) != 0) abort();
}
static void zone_unlock(void)
{
    if (pthread_mutex_unlock(&s_zone_lock) != 0) abort();
}

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

/* "[+-]hh[:mm[:ss]]" in POSIX sign convention (west positive) -> seconds EAST
 * of UTC. Advances *p. Returns false when no offset is present. */
static bool posix_offset(const char **p, int32_t *east)
{
    const char *s = *p;
    int sign = 1;
    if (*s == '+' || *s == '-') sign = (*s++ == '-') ? -1 : 1;
    if (*s < '0' || *s > '9') return false;
    int32_t parts[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        int32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) v = v * 10 + (*s++ - '0'), digits++;
        if (digits == 0) return false;
        parts[i] = v;
        if (*s != ':' || i == 2) break;
        s++;
    }
    *east = -sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]);
    *p = s;
    return true;
}

/* Abbreviation: three or more letters, or <...> (may hold digits and a sign). */
static bool posix_abbr(const char **p)
{
    const char *s = *p;
    if (*s == '<') {
        const char *close = strchr(s, '>');
        if (close == NULL || close == s + 1) return false;
        *p = close + 1;
        return true;
    }
    int n = 0;
    while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) s++, n++;
    if (n < 3) return false;
    *p = s;
    return true;
}

/* The two offsets a POSIX rule can ever produce: standard and (if present)
 * daylight time, east of UTC. DST defaults to standard + 1 h, as POSIX says. */
static bool posix_offsets(const char *rule, int32_t *std_east, int32_t *dst_east, bool *has_dst)
{
    const char *p = rule;
    if (!posix_abbr(&p) || !posix_offset(&p, std_east)) return false;
    *has_dst = false;
    *dst_east = *std_east;
    if (*p == '\0') return true;
    if (!posix_abbr(&p)) return false;
    *has_dst = true;
    *dst_east = *std_east + 3600;
    if (*p != ',' && *p != '\0' && !posix_offset(&p, dst_east)) return false;
    return true;
}

/* Is libc now running exactly `rule`?
 *
 * Two independent failures used to be invisible here. setenv() can fail
 * (ENOMEM on a fragmented heap) and leave the PREVIOUS rule installed; and
 * newlib's tzset() parses leniently, silently ending the parse at a field it
 * cannot scan. The old check only asked whether the offset was non-zero, so a
 * Berlin rule left in place by a failed setenv "passed" a Darwin probe and the
 * worker reported applied=true at +3600 (gpt-6-astra review of #98). Now: TZ
 * must hold exactly the rule string, and at both probe instants -- half a year
 * apart, one per DST season -- libc's offset must be one this rule can produce
 * (its standard or daylight offset). A leftover rule from another zone, or a
 * half-parsed one falling back to UTC, cannot satisfy that for a non-UTC zone. */
static bool rule_installed(const char *rule)
{
    const char *env = getenv("TZ");
    if (env == NULL || strcmp(env, rule) != 0) return false;
    int32_t std_east, dst_east;
    bool has_dst;
    if (!posix_offsets(rule, &std_east, &dst_east, &has_dst)) return false;
    static const int64_t k_probes[2] = {
        1768478400,   /* 2026-01-15 12:00 UTC */
        1784116800,   /* 2026-07-15 12:00 UTC */
    };
    for (size_t i = 0; i < sizeof(k_probes) / sizeof(k_probes[0]); i++) {
        int32_t off = libc_offset_seconds(k_probes[i]);
        if (off != std_east && !(has_dst && off == dst_east)) return false;
    }
    return true;
}

/* POSIX rule currently installed AND verified, or NULL. Lock-protected. */
static const char *s_rule = NULL;

static esp_err_t apply_locked(const char *iana)
{
    char canonical[48];
    esp_err_t canonical_err = timezone_canonicalize(iana, canonical, sizeof(canonical));
    if (canonical_err != ESP_OK) {
        s_applied = false;
        atomic_store(&s_zone_name, "");
        ESP_LOGE(TAG, "rejecting invalid timezone '%s'", iana != NULL ? iana : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    const tz_zone_t *zone = zone_lookup(canonical);
    if (zone == NULL) {
        s_applied = false;
        atomic_store(&s_zone_name, "");
        ESP_LOGI(TAG, "no timezone configured — scheduler uses the fallback offset");
        return ESP_OK;
    }
    const char *posix = k_tz_rules[zone->rule];
    if (setenv("TZ", posix, 1) != 0) {
        /* Nothing changed in libc: keep the previous identity, rule and
         * offset, and say so. A retry is not skipped (see the worker). */
        ESP_LOGE(TAG, "setenv(TZ) failed for '%s'; keeping '%s'", canonical,
                 atomic_load(&s_zone_name));
        return ESP_ERR_NO_MEM;
    }
    tzset();
    if (rule_installed(posix)) {
        s_applied = true;
        s_rule = posix;
        atomic_store(&s_zone_name, zone->iana);
        ESP_LOGI(TAG, "timezone '%s' -> TZ=%s (DST-aware scheduling, tzdata %s)",
                 canonical, posix, TZ_TABLE_TZDATA_VERSION);
        return ESP_OK;
    }
    /* libc did not take the new rule. Put the previous verified rule back, so
     * the device keeps running its previous zone instead of a half-state. */
    if (s_rule != NULL && setenv("TZ", s_rule, 1) == 0) {
        tzset();
        if (rule_installed(s_rule)) {
            ESP_LOGE(TAG, "libc rejected TZ=%s for '%s'; kept '%s'", posix, canonical,
                     atomic_load(&s_zone_name));
            return ESP_ERR_INVALID_STATE;
        }
    }
    /* No verified rule at all (first apply at boot, or the restore failed too):
     * keep scheduling on the zone's own prevailing offset rather than the
     * unrelated time_sync default -- wrong by at most the DST hour, not by the
     * whole distance from Amsterdam. Reported as not applied. */
    s_applied = false;
    s_rule = NULL;
    atomic_store(&s_zone_name, zone->iana);
    time_sync_set_utc_offset_seconds((int32_t)zone->fallback_min * 60);
    ESP_LOGE(TAG, "libc rejected TZ=%s for '%s' — scheduling on the fixed "
                  "offset %d min, DST will NOT be tracked",
             posix, canonical, (int)zone->fallback_min);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t timezone_apply(const char *iana)
{
    zone_lock();
    esp_err_t err = apply_locked(iana);
    /* Solar calculations also consume this offset, including half-hour zones.
     * Keep that cache current without moving the RTC or any UTC timestamp. */
    if (s_applied) time_sync_set_utc_offset_seconds(libc_offset_seconds((int64_t)time(NULL)));
    zone_unlock();
    return err;
}

bool timezone_rule_active(void)
{
    zone_lock();
    bool active = s_applied;
    zone_unlock();
    return active;
}

const char *timezone_country(const char *iana)
{
    const tz_zone_t *zone = zone_lookup(iana);
    return zone != NULL ? k_tz_countries[zone - k_tz_zones] : "";
}

const char *timezone_current_name(void)
{
    return atomic_load(&s_zone_name);
}

void timezone_snapshot(int64_t utc, const char **name, int32_t *offset)
{
    zone_lock();
    if (name) *name = atomic_load(&s_zone_name);
    if (offset) *offset = s_applied ? libc_offset_seconds(utc) : time_sync_get_utc_offset_seconds();
    zone_unlock();
}

int32_t timezone_utc_offset_seconds(int64_t utc)
{
    int32_t offset;
    timezone_snapshot(utc, NULL, &offset);
    return offset;
}

/* Kept within the same lock as apply: an old reader must not overwrite the
 * solar offset cache after a new zone has been installed. */
int64_t timezone_localize(int64_t utc)
{
    zone_lock();
    int32_t off = s_applied ? libc_offset_seconds(utc) : time_sync_get_utc_offset_seconds();
    time_sync_set_utc_offset_seconds(off);
    zone_unlock();
    return utc + off;
}
