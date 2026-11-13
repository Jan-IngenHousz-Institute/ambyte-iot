/*
 * device_location.c — effective-location precedence and live-estimate
 * acceptance (see device_location.h for the rules and their reasons).
 *
 * State: the stored live fix (RAM, mirrored to one NVS blob) and the
 * unconfirmed change candidate for a live-derived zone (RAM only: the
 * 3-estimate / 6 h confirmation restarts after a reboot, which errs toward
 * NOT changing the zone).
 *
 * Locking (Opus review of #100): s_lock guards the fix and candidate and is
 * never held across an NVS WRITE. Offers come only from the maintenance
 * worker, which prepares the new fix under the lock, writes NVS without it,
 * then commits the RAM copy. Short NVS READS remain under the lock (the
 * configured coordinates and the loc_live mode, via device_config), so a
 * reader can wait for those reads but never for a flash write or erase. The
 * live status string has its own short lock because the esp-mqtt task sets
 * it (SUBACK refusals).
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
    device_live_fix_t f;
} fix_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_status_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_loaded;
static bool s_expired_reported;
static fix_t s_live;
static struct {
    bool has;
    const char *zone;    /* map-owned, lifetime of the firmware */
    int count;
    int64_t first_utc;
} s_candidate;
static char s_status[24] = "off";

static void lock(pthread_mutex_t *m) { if (pthread_mutex_lock(m) != 0) abort(); }
static void unlock(pthread_mutex_t *m) { if (pthread_mutex_unlock(m) != 0) abort(); }

/* Country of a coordinate via the on-device map + tzdata zone.tab; "" when
 * it resolves to no zone or a zone without a country. */
static const char *country_at(double lat, double lon)
{
    const char *zone = tz_geo_zone_for(lat, lon);
    return zone != NULL ? timezone_country(zone) : "";
}

static void load_locked(void)
{
    if (s_loaded) return;
    s_loaded = true;
    device_live_fix_t f;
    if (device_config_get_live_fix(&f) == ESP_OK && tz_geo_location_usable(f.lat, f.lon)) {
        s_live = (fix_t){.has = true, .this_boot = false, .f = f};
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

static bool fresh(const fix_t *fx, int64_t now_utc)
{
    if (!fx->has) return false;
    /* Without a trusted clock the age is unknowable: a fix accepted during
     * this boot is still good, a persisted one is not. */
    if (now_utc < DEVICE_LOCATION_TRUSTED_UTC) return fx->this_boot;
    int64_t age = now_utc - fx->f.fix_utc;
    return age >= -86400 && age <= DEVICE_LOCATION_MAX_AGE_S;
}

static bool usable_locked(int64_t now_utc)
{
    return live_enabled() && fresh(&s_live, now_utc);
}

static bool confirmed(const device_live_fix_t *f)
{
    return f->agree_count >= DEVICE_LOCATION_AGREE_COUNT &&
           f->fix_utc - f->agree_first_utc >= DEVICE_LOCATION_AGREE_SPAN_S;
}

bool device_location_configured(device_location_t *out)
{
    double lat, lon;
    if (device_config_get_location(&lat, &lon, NULL, 0) != ESP_OK ||
        !tz_geo_location_usable(lat, lon)) {
        return false;
    }
    if (out) {
        *out = (device_location_t){DEVICE_LOCATION_CONFIGURED, lat, lon, NAN, -1, false,
                                   DEVICE_LIVE_SOURCE_NONE};
    }
    return true;
}

static device_location_t live_out(const fix_t *fx, int64_t now_utc)
{
    int64_t age = now_utc >= DEVICE_LOCATION_TRUSTED_UTC ? now_utc - fx->f.fix_utc : -1;
    return (device_location_t){DEVICE_LOCATION_LIVE, fx->f.lat, fx->f.lon, fx->f.accuracy_m,
                               age < 0 ? -1 : age, false, (device_live_source_t)fx->f.source};
}

bool device_location_live(device_location_t *out, int64_t now_utc)
{
    lock(&s_lock);
    load_locked();
    bool ok = usable_locked(now_utc);
    if (ok && out) *out = live_out(&s_live, now_utc);
    unlock(&s_lock);
    return ok;
}

/* Would this live fix be the effective location, given `cfg` (or none)? */
static bool live_wins(const device_live_fix_t *f, const device_location_t *cfg, bool have_cfg,
                      bool *stale)
{
    *stale = false;
    if (!have_cfg) {
        /* No configured site: a Wi-Fi fix wins at once; an IP fix only once two
         * agreeing answers an hour apart rule out a one-off wrong egress. */
        return f->source == DEVICE_LIVE_SOURCE_WIFI || confirmed(f);
    }
    const char *cc_cfg = country_at(cfg->lat, cfg->lon);
    if (cc_cfg[0] && strcmp(cc_cfg, country_at(f->lat, f->lon)) == 0) {
        return false;   /* same country: the precise configured site wins */
    }
    /* Another country: the configured site looks like a stale preset. Only a
     * confirmed Wi-Fi fix may override it; an IP fix (CGNAT, roaming SIM,
     * spoofable lookup) never does -- it is reported for an operator. */
    *stale = true;
    return f->source == DEVICE_LIVE_SOURCE_WIFI && confirmed(f);
}

/* The rules in device_location.h. Caller holds s_lock. */
static device_location_source_t effective_locked(device_location_t *out, int64_t now_utc)
{
    device_location_t cfg;
    bool have_cfg = device_location_configured(&cfg);
    device_location_t loc = {DEVICE_LOCATION_NONE, 0, 0, NAN, -1, false, DEVICE_LIVE_SOURCE_NONE};
    if (have_cfg) loc = cfg;
    if (usable_locked(now_utc)) {
        bool stale;
        if (live_wins(&s_live.f, &cfg, have_cfg, &stale)) {
            loc = live_out(&s_live, now_utc);
        }
        loc.configured_stale = stale;
    }
    if (out) *out = loc;
    return loc.source;
}

device_location_source_t device_location_effective(device_location_t *out, int64_t now_utc)
{
    lock(&s_lock);
    load_locked();
    device_location_source_t source = effective_locked(out, now_utc);
    unlock(&s_lock);
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

const char *device_location_live_source_name(device_live_source_t source)
{
    switch (source) {
    case DEVICE_LIVE_SOURCE_WIFI: return "wifi";
    case DEVICE_LIVE_SOURCE_IP:   return "ip";
    default:                      return NULL;
    }
}

static double distance_m(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371000.0, rad = 3.14159265358979323846 / 180.0;
    double x = (lon2 - lon1) * rad * cos((lat1 + lat2) * 0.5 * rad);
    double y = (lat2 - lat1) * rad;
    return r * sqrt(x * x + y * y);
}

/* The fix that adopting this estimate would store, and whether it must be
 * written to NVS. Caller holds s_lock.
 *
 * Agreement: an estimate of the same source in the same zone as the stored
 * fix counts towards its confirmation (count up, first time kept). Position
 * hysteresis (solar triggers re-resolve whenever the position moves): within
 * the zone the position changes only beyond max(1 km, the new estimate's own
 * uncertainty), or for an estimate at least twice as precise. A different zone, or a
 * different source that moves the position, replaces the fix and restarts
 * the count; a different source inside the noise only refreshes the age. */
static device_live_fix_t next_fix_locked(const device_live_fix_t *in, const char *zone,
                                         bool zone_change, bool *persist)
{
    device_live_fix_t out = *in;
    *persist = true;
    if (!s_live.has || zone_change || tz_geo_zone_for(s_live.f.lat, s_live.f.lon) != zone) {
        return out;
    }
    const device_live_fix_t *old = &s_live.f;
    double d = distance_m(old->lat, old->lon, in->lat, in->lon);
    /* An estimate that lands inside its OWN uncertainty of the stored fix
     * agrees with it and moves nothing: a +-5 km IP answer 2 km from a +-41 m
     * Wi-Fi fix must not replace it (found on the bench, 2026-11-13). It
     * moves the fix only beyond max(1 km, its own uncertainty), or when it is
     * at least twice as precise and over 1 km away. */
    double noise = fmax(DEVICE_LOCATION_PERSIST_MOVE_M, in->accuracy_m);
    bool sharper = in->accuracy_m * 2.0 <= old->accuracy_m && d > DEVICE_LOCATION_PERSIST_MOVE_M;
    bool moves = d > noise || sharper;
    if (in->source != old->source) {
        if (moves) return out;          /* a better, different source takes over */
        out = *old;                     /* a different source inside the noise */
        out.fix_utc = in->fix_utc;
        *persist = false;
        return out;
    }
    out = *old;
    if (moves) {
        out.lat = in->lat;
        out.lon = in->lon;
        out.accuracy_m = in->accuracy_m;
    }
    out.fix_utc = in->fix_utc;
    if (out.agree_count < 255) out.agree_count++;
    /* The count is what lets an IP / overriding fix win, so it is written
     * until the fix is confirmed; after that only a real move writes (flash
     * wear: a settled unit stops writing). */
    *persist = moves || !confirmed(old);
    return out;
}

device_location_offer_t device_location_offer_live(double lat, double lon, double accuracy_m,
                                                   int64_t fix_utc, int64_t now_utc,
                                                   device_live_source_t source)
{
    if (!tz_geo_location_usable(lat, lon)) return DEVICE_LOCATION_OFFER_NO_ZONE;
    if (!isfinite(accuracy_m) || accuracy_m < 0.0 || accuracy_m > DEVICE_LOCATION_MAX_ACCURACY_M) {
        return DEVICE_LOCATION_OFFER_INACCURATE;
    }
    if (source != DEVICE_LIVE_SOURCE_WIFI && source != DEVICE_LIVE_SOURCE_IP) {
        return DEVICE_LOCATION_OFFER_INACCURATE;
    }
    const char *zone = tz_geo_zone_for(lat, lon);
    if (zone == NULL || timezone_country(zone)[0] == '\0') return DEVICE_LOCATION_OFFER_NO_ZONE;
    device_live_fix_t in = {.lat = lat, .lon = lon, .accuracy_m = accuracy_m, .fix_utc = fix_utc,
                            .agree_first_utc = fix_utc, .source = (uint8_t)source, .agree_count = 1};

    /* 1. Decide under the lock. */
    lock(&s_lock);
    load_locked();
    if (source == DEVICE_LIVE_SOURCE_IP && fresh(&s_live, now_utc) &&
        s_live.f.source == DEVICE_LIVE_SOURCE_WIFI &&
        strcmp(timezone_country(zone), country_at(s_live.f.lat, s_live.f.lon)) == 0) {
        /* IP is country evidence only (Opus review of #100). Inside the
         * country of a Wi-Fi fix it confirms the country and refreshes the
         * fix's age, nothing more: no move, no candidate. Otherwise a period
         * of Wi-Fi 404s would walk a precise zone to the carrier's egress
         * city (Uluru -> Adelaide) after 3 agreeing IP answers. */
        s_live.f.fix_utc = fix_utc;
        s_live.this_boot = true;
        unlock(&s_lock);
        return DEVICE_LOCATION_OFFER_ACCEPTED;
    }
    bool live_driving = effective_locked(NULL, now_utc) == DEVICE_LOCATION_LIVE;
    const char *live_zone = live_driving ? tz_geo_zone_for(s_live.f.lat, s_live.f.lon) : NULL;
    bool zone_change = false;
    device_location_offer_t result = DEVICE_LOCATION_OFFER_ACCEPTED;
    if (live_zone != NULL && strcmp(live_zone, zone) != 0) {
        /* Hysteresis guards only a zone that is ALREADY live-derived. */
        if (!s_candidate.has || strcmp(s_candidate.zone, zone) != 0) {
            s_candidate.has = true;
            s_candidate.zone = zone;
            s_candidate.count = 0;
            s_candidate.first_utc = now_utc;
        }
        s_candidate.count++;
        if (s_candidate.count < DEVICE_LOCATION_CONFIRMATIONS ||
            now_utc - s_candidate.first_utc < DEVICE_LOCATION_CONFIRM_SPAN_S) {
            unlock(&s_lock);
            return DEVICE_LOCATION_OFFER_CANDIDATE;
        }
        zone_change = true;
        result = DEVICE_LOCATION_OFFER_ZONE_CHANGE;
    }
    s_candidate.has = false;
    bool persist;
    device_live_fix_t next = next_fix_locked(&in, zone, zone_change, &persist);
    if (zone_change) {
        /* The confirmed change carries its confirmations. */
        next.agree_count = DEVICE_LOCATION_CONFIRMATIONS;
        next.agree_first_utc = next.fix_utc - DEVICE_LOCATION_CONFIRM_SPAN_S;
    }
    unlock(&s_lock);

    /* 2. Flash write without the lock (worker task only: no other writer). */
    if (persist && device_config_set_live_fix(&next) != ESP_OK) {
        return DEVICE_LOCATION_OFFER_PERSIST_FAILED;
    }

    /* 3. Commit the RAM copy. */
    lock(&s_lock);
    s_live = (fix_t){.has = true, .this_boot = true, .f = next};
    s_expired_reported = false;
    unlock(&s_lock);
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

bool device_location_check_expiry(int64_t now_utc)
{
    lock(&s_lock);
    load_locked();
    bool expired = s_live.has && !s_expired_reported && now_utc >= DEVICE_LOCATION_TRUSTED_UTC &&
                   !fresh(&s_live, now_utc);
    if (expired) s_expired_reported = true;
    unlock(&s_lock);
    if (expired) device_location_set_live_status("expired");
    return expired;
}

void device_location_set_live_status(const char *status)
{
    lock(&s_status_lock);
    snprintf(s_status, sizeof s_status, "%s", status ? status : "");
    unlock(&s_status_lock);
}

void device_location_live_status(char *out, size_t len)
{
    if (out == NULL || len == 0) return;
    lock(&s_status_lock);
    snprintf(out, len, "%s", s_status);
    unlock(&s_status_lock);
}

void device_location_reset_for_test(void)
{
    lock(&s_lock);
    s_loaded = false;
    s_expired_reported = false;
    memset(&s_live, 0, sizeof s_live);
    memset(&s_candidate, 0, sizeof s_candidate);
    unlock(&s_lock);
    device_location_set_live_status("off");
}
