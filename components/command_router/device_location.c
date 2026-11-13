/*
 * device_location.c — effective-location precedence and live-estimate
 * acceptance (see device_location.h for the rules and their reasons).
 *
 * State: the accepted live fix (RAM, mirrored to one NVS blob on a zone change
 * or > 1 km movement) and the unconfirmed candidate (RAM only: the 3-estimate /
 * 6 h confirmation restarts after a reboot, which errs toward NOT changing
 * the zone). Offers come from the maintenance worker; identity and STATUS
 * read concurrently, hence the mutex.
 */
#include "device_location.h"

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_config.h"
#include "timezone.h"
#include "tz_geo.h"

typedef struct {
    bool has;
    bool this_boot;      /* accepted since boot: usable without a trusted clock */
    double lat, lon, acc;
    int64_t fix_utc;
} fix_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_loaded;
static fix_t s_live;
static struct {
    bool has;
    const char *zone;    /* map-owned, lifetime of the firmware */
    int count;
    int64_t first_utc;
} s_candidate;

/* Country of a coordinate via the on-device map + tzdata zone.tab; "" when
 * it resolves to no zone or a zone without a country. */
static const char *country_at(double lat, double lon)
{
    const char *zone = tz_geo_zone_for(lat, lon);
    return zone != NULL ? timezone_country(zone) : "";
}
static char s_status[24] = "off";

static void lock(void) { if (pthread_mutex_lock(&s_lock) != 0) abort(); }
static void unlock(void) { if (pthread_mutex_unlock(&s_lock) != 0) abort(); }

static void load_locked(void)
{
    if (s_loaded) return;
    s_loaded = true;
    double lat, lon, acc;
    int64_t when;
    if (device_config_get_live_fix(&lat, &lon, &acc, &when) == ESP_OK &&
        tz_geo_location_usable(lat, lon)) {
        s_live = (fix_t){true, false, lat, lon, acc, when};
    }
}

/* `cfg set loc_live off` turns the live source off entirely: the persisted
 * fix stops counting too, so position and zone fall back to configured. */
static bool live_enabled(void)
{
    char mode[8];
    return device_config_get_live_location_mode(mode, sizeof mode) != ESP_OK ||
           strcmp(mode, "off") != 0;
}

static bool usable_locked(int64_t now_utc)
{
    if (!s_live.has || !live_enabled()) return false;
    /* Without a trusted clock the age is unknowable: a fix accepted during
     * this boot is still good, a persisted one is not. */
    if (now_utc < DEVICE_LOCATION_TRUSTED_UTC) return s_live.this_boot;
    int64_t age = now_utc - s_live.fix_utc;
    return age >= -86400 && age <= DEVICE_LOCATION_MAX_AGE_S;
}

bool device_location_configured(device_location_t *out)
{
    double lat, lon;
    if (device_config_get_location(&lat, &lon, NULL, 0) != ESP_OK ||
        !tz_geo_location_usable(lat, lon)) {
        return false;
    }
    if (out) *out = (device_location_t){DEVICE_LOCATION_CONFIGURED, lat, lon, NAN, -1, false};
    return true;
}

static void live_out_locked(device_location_t *out, int64_t now_utc)
{
    int64_t age = now_utc >= DEVICE_LOCATION_TRUSTED_UTC ? now_utc - s_live.fix_utc : -1;
    *out = (device_location_t){DEVICE_LOCATION_LIVE, s_live.lat, s_live.lon, s_live.acc,
                               age < 0 ? -1 : age, false};
}

bool device_location_live(device_location_t *out, int64_t now_utc)
{
    lock();
    load_locked();
    bool ok = usable_locked(now_utc);
    if (ok && out) live_out_locked(out, now_utc);
    unlock();
    return ok;
}

/* The country rule (device_location.h). Caller holds the lock. */
static device_location_source_t effective_locked(device_location_t *out, int64_t now_utc)
{
    device_location_t cfg;
    bool have_cfg = device_location_configured(&cfg);
    bool have_live = usable_locked(now_utc);
    device_location_t loc = {DEVICE_LOCATION_NONE, 0, 0, NAN, -1, false};
    if (have_live) {
        live_out_locked(&loc, now_utc);
        if (have_cfg) {
            const char *cc_cfg = country_at(cfg.lat, cfg.lon);
            if (cc_cfg[0] && strcmp(cc_cfg, country_at(s_live.lat, s_live.lon)) == 0) {
                loc = cfg;   /* same country: the precise configured location wins */
            } else {
                loc.configured_stale = true;
            }
        }
    } else if (have_cfg) {
        loc = cfg;
    }
    if (out) *out = loc;
    return loc.source;
}

device_location_source_t device_location_effective(device_location_t *out, int64_t now_utc)
{
    lock();
    load_locked();
    device_location_source_t source = effective_locked(out, now_utc);
    unlock();
    return source;
}

const char *device_location_source_name(device_location_source_t source)
{
    switch (source) {
    case DEVICE_LOCATION_LIVE:       return "live";
    case DEVICE_LOCATION_CONFIGURED: return "configured";
    default:                         return "none";
    }
}

static double distance_m(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371000.0, rad = 3.14159265358979323846 / 180.0;
    double x = (lon2 - lon1) * rad * cos((lat1 + lat2) * 0.5 * rad);
    double y = (lat2 - lat1) * rad;
    return r * sqrt(x * x + y * y);
}

/* Adopt (lat, lon) as the live fix; persist when required.
 *
 * Position hysteresis (solar triggers are re-resolved whenever the position
 * moves): within the same zone, an estimate replaces the fix only when it
 * moved more than both 1 km and the smaller of the two uncertainties, or is
 * at least twice as precise. Repeated IP answers wandering a few km inside
 * their +-5..50 km never move sunrise; a Wi-Fi fix replacing an IP one does. */
static bool adopt_locked(double lat, double lon, double acc, int64_t fix_utc, bool zone_change)
{
    bool moved = true;
    if (s_live.has && !zone_change) {
        double d = distance_m(s_live.lat, s_live.lon, lat, lon);
        double noise = DEVICE_LOCATION_PERSIST_MOVE_M;
        double smaller = fmin(isfinite(acc) ? acc : INFINITY, isfinite(s_live.acc) ? s_live.acc : INFINITY);
        if (isfinite(smaller) && smaller > noise) noise = smaller;
        bool sharper = isfinite(acc) && isfinite(s_live.acc) && acc * 2.0 <= s_live.acc &&
                       d > DEVICE_LOCATION_PERSIST_MOVE_M;
        moved = d > noise || sharper;
    }
    bool persist = zone_change || !s_live.has || moved;
    if (persist && device_config_set_live_fix(lat, lon, isfinite(acc) ? acc : 0.0, fix_utc) != ESP_OK) {
        return false;
    }
    if (persist) {
        s_live = (fix_t){true, true, lat, lon, acc, fix_utc};
    } else {
        /* Same place within the noise: refresh freshness in RAM only (flash
         * wear); position and accuracy stay, so nothing re-anchors. */
        s_live.this_boot = true;
        s_live.fix_utc = fix_utc;
    }
    return true;
}

device_location_offer_t device_location_offer_live(double lat, double lon, double accuracy_m,
                                                   int64_t fix_utc, int64_t now_utc)
{
    if (!tz_geo_location_usable(lat, lon)) return DEVICE_LOCATION_OFFER_NO_ZONE;
    if (!isfinite(accuracy_m) || accuracy_m < 0.0 || accuracy_m > DEVICE_LOCATION_MAX_ACCURACY_M) {
        return DEVICE_LOCATION_OFFER_INACCURATE;
    }
    const char *zone = tz_geo_zone_for(lat, lon);
    if (zone == NULL || timezone_country(zone)[0] == '\0') return DEVICE_LOCATION_OFFER_NO_ZONE;
    device_location_offer_t result;
    lock();
    load_locked();
    /* Hysteresis only guards a zone that is ALREADY live-derived. */
    device_location_t eff;
    bool live_driving = effective_locked(&eff, now_utc) == DEVICE_LOCATION_LIVE;
    const char *live_zone = live_driving ? tz_geo_zone_for(s_live.lat, s_live.lon) : NULL;
    if (live_zone == NULL || strcmp(live_zone, zone) == 0) {
        s_candidate.has = false;
        /* Persist on a different zone than the stored fix (names are
         * map-owned, so pointer equality is name equality) or > 1 km. */
        bool zone_change = !s_live.has || tz_geo_zone_for(s_live.lat, s_live.lon) != zone;
        result = adopt_locked(lat, lon, accuracy_m, fix_utc, zone_change)
                 ? DEVICE_LOCATION_OFFER_ACCEPTED : DEVICE_LOCATION_OFFER_PERSIST_FAILED;
    } else {
        if (!s_candidate.has || strcmp(s_candidate.zone, zone) != 0) {
            s_candidate.has = true;
            s_candidate.zone = zone;
            s_candidate.count = 0;
            s_candidate.first_utc = now_utc;
        }
        s_candidate.count++;
        if (s_candidate.count >= DEVICE_LOCATION_CONFIRMATIONS &&
            now_utc - s_candidate.first_utc >= DEVICE_LOCATION_CONFIRM_SPAN_S) {
            s_candidate.has = false;
            result = adopt_locked(lat, lon, accuracy_m, fix_utc, true)
                     ? DEVICE_LOCATION_OFFER_ZONE_CHANGE : DEVICE_LOCATION_OFFER_PERSIST_FAILED;
        } else {
            result = DEVICE_LOCATION_OFFER_CANDIDATE;
        }
    }
    unlock();
    return result;
}

const char *device_location_offer_name(device_location_offer_t offer)
{
    switch (offer) {
    case DEVICE_LOCATION_OFFER_ACCEPTED:       return "ok";
    case DEVICE_LOCATION_OFFER_ZONE_CHANGE:    return "zone_change";
    case DEVICE_LOCATION_OFFER_CANDIDATE:      return "candidate";
    case DEVICE_LOCATION_OFFER_INACCURATE:     return "inaccurate";
    case DEVICE_LOCATION_OFFER_NO_ZONE:        return "no_zone";
    case DEVICE_LOCATION_OFFER_PERSIST_FAILED: return "persist_failed";
    default:                                   return "unknown";
    }
}

void device_location_set_live_status(const char *status)
{
    lock();
    snprintf(s_status, sizeof s_status, "%s", status ? status : "");
    unlock();
}

void device_location_live_status(char *out, size_t len)
{
    if (out == NULL || len == 0) return;
    lock();
    snprintf(out, len, "%s", s_status);
    unlock();
}

void device_location_reset_for_test(void)
{
    lock();
    s_loaded = false;
    memset(&s_live, 0, sizeof s_live);
    memset(&s_candidate, 0, sizeof s_candidate);
    snprintf(s_status, sizeof s_status, "off");
    unlock();
}
