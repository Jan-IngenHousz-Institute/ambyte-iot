#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the device is, and how it knows (docs/timezone-from-location.md).
 *
 * The Ambyte has no GPS. Two sources:
 *   configured  persisted coordinates: provisioning image (GUI), MQTT
 *               set_location or USB `sync loc`. Precise, but often a preset
 *               from wherever the unit was prepared (Uluru units: Europe).
 *   live        an accepted online estimate (AWS IoT Device Location over the
 *               MQTT connection). Coarse: a cellular site's IP resolves to its
 *               CGNAT egress city (Uluru -> Adelaide, 1,285 km, +-50 km).
 * Which wins is decided by COUNTRY (each coordinate -> zone -> ISO country,
 * tzdata zone.tab, timezone_country):
 *   no configured location                    -> live
 *   configured in another country than live   -> live; configured is a stale
 *                                                preset (configured_stale)
 *   configured in the same country            -> configured (more precise; an
 *                                                in-country egress offset is the
 *                                                expected live error)
 *   neither                                   -> none
 * A timezone pin overrides all of this (timezone_command). time_sync's
 * compiled 52.173N 5.819E sun-math default is NOT a location and is never
 * reported as one. (0, 0) is treated as unset. */

typedef enum {
    DEVICE_LOCATION_NONE = 0,
    DEVICE_LOCATION_CONFIGURED,
    DEVICE_LOCATION_LIVE,
} device_location_source_t;

typedef struct {
    device_location_source_t source;
    double lat;
    double lon;
    double accuracy_m;      /* NAN when unknown (configured coordinates) */
    int64_t age_s;          /* -1 when unknown */
    bool configured_stale;  /* effective only: live won over a configured
                               location in another country */
} device_location_t;

/* Live-estimate acceptance (approved 2026-11-13, calibrated on field data):
 *  - horizontal accuracy must be reported and <= 200 km: a sanity floor only.
 *    Real IP estimates are +-5 km (office) to +-50 km (Uluru cellular); the
 *    country rule above does the real work.
 *  - the estimate must resolve to a zone with a country (not open water).
 *  - while the zone in effect is ALREADY live-derived, changing it needs 3
 *    agreeing estimates (same zone) spanning >= 6 h, so a noisy source cannot
 *    flap the zone and re-anchor the scheduler. Replacing a configured- or
 *    fallback-derived zone takes effect on the first accepted estimate.
 *  - within a zone, a new estimate moves the fix (and with it the solar
 *    position) only beyond max(1 km, smaller uncertainty), or when it is at
 *    least twice as precise -- IP noise never re-anchors sunrise/sunset.
 *  - `loc_live off` ignores the live fix entirely (configured / none).
 *  - the accepted fix persists (NVS) only on a zone change or a real move,
 *    and is used while younger than 30 days (with an untrusted clock only a
 *    fix accepted during this boot is used). */
#define DEVICE_LOCATION_MAX_ACCURACY_M   200000.0
#define DEVICE_LOCATION_CONFIRMATIONS    3
#define DEVICE_LOCATION_CONFIRM_SPAN_S   (6 * 3600)
#define DEVICE_LOCATION_MAX_AGE_S        (30 * 86400)
#define DEVICE_LOCATION_PERSIST_MOVE_M   1000.0
/* Any UTC earlier than this means the clock is not trusted (2024-01-01). */
#define DEVICE_LOCATION_TRUSTED_UTC      1704067200LL

/* The persisted fallback coordinates. False when unset/unreadable/(0,0). */
bool device_location_configured(device_location_t *out);
/* The accepted live estimate, if still usable at `now_utc` (whether or not it
 * currently wins). */
bool device_location_live(device_location_t *out, int64_t now_utc);
/* The country rule above; `out->source` says which (NONE: lat/lon unset). */
device_location_source_t device_location_effective(device_location_t *out, int64_t now_utc);
/* "live" | "configured" | "none" */
const char *device_location_source_name(device_location_source_t source);

typedef enum {
    DEVICE_LOCATION_OFFER_ACCEPTED = 0,   /* now the live location */
    DEVICE_LOCATION_OFFER_ZONE_CHANGE,    /* confirmed change of a live-derived zone */
    DEVICE_LOCATION_OFFER_CANDIDATE,      /* would change a live zone; not yet confirmed */
    DEVICE_LOCATION_OFFER_INACCURATE,     /* accuracy missing or worse than 200 km */
    DEVICE_LOCATION_OFFER_NO_ZONE,        /* resolves to no zone (open water) */
    DEVICE_LOCATION_OFFER_PERSIST_FAILED, /* NVS write failed: not adopted */
} device_location_offer_t;

/* Offer one live estimate (already validated by its source). */
device_location_offer_t device_location_offer_live(double lat, double lon, double accuracy_m,
                                                   int64_t fix_utc, int64_t now_utc);
const char *device_location_offer_name(device_location_offer_t offer);

/* Last live-source outcome for telemetry ("off", "ok", "no_fix",
 * "unreachable", "candidate", ...). Copied; at most 23 chars. */
void device_location_set_live_status(const char *status);
void device_location_live_status(char *out, size_t len);

/* Host tests only: forget RAM state (as a reboot does). */
void device_location_reset_for_test(void);

#ifdef __cplusplus
}
#endif
