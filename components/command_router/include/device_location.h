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
 *               MQTT connection), from a Wi-Fi scan or from the public IP.
 *               IP estimates are coarse and can be far off: a cellular
 *               site's CGNAT egress (Uluru -> Adelaide, 1,285 km), a SIM
 *               breaking out in another country, or a spoofed plain-HTTP IP.
 *
 * Which wins (each coordinate -> zone -> ISO country via tzdata zone.tab):
 *   no configured location     -> live, if it is a Wi-Fi fix, or an IP fix
 *                                 confirmed by 2 agreeing estimates >= 1 h apart
 *   configured, same country   -> configured (more precise)
 *   configured, other country  -> configured_stale is reported; live wins only
 *                                 if it is a Wi-Fi fix confirmed by 2 agreeing
 *                                 Wi-Fi estimates >= 1 h apart. An IP fix NEVER
 *                                 overrides a configured location.
 *   neither                    -> none
 * A timezone pin overrides the zone (timezone_command); the position still
 * follows the effective location. time_sync's compiled 52.173N 5.819E sun-math
 * default is NOT a location and is never reported as one. (0, 0) is unset. */

typedef enum {
    DEVICE_LOCATION_NONE = 0,
    DEVICE_LOCATION_CONFIGURED,
    DEVICE_LOCATION_LIVE,
} device_location_source_t;

typedef enum {
    DEVICE_LIVE_SOURCE_NONE = 0,
    DEVICE_LIVE_SOURCE_WIFI = 1,
    DEVICE_LIVE_SOURCE_IP = 2,
} device_live_source_t;

typedef struct {
    device_location_source_t source;
    double lat;
    double lon;
    double accuracy_m;      /* NAN when unknown (configured coordinates) */
    int64_t age_s;          /* -1 when unknown */
    bool configured_stale;  /* effective only: a live fix in another country
                               than the configured location (reported whether
                               or not it won) */
    device_live_source_t live_source;  /* live fixes: wifi | ip */
} device_location_t;

/* Live-estimate acceptance (approved 2026-11-13, calibrated on field data):
 *  - horizontal accuracy must be reported and <= 200 km: a sanity floor only
 *    (IP answers are +-5..50 km); the rules above do the real work.
 *  - the estimate must resolve to a zone with a country (not open water).
 *  - while the zone in effect is ALREADY live-derived, changing it needs 3
 *    agreeing estimates (same zone) spanning >= 6 h.
 *  - an IP estimate in the same country as a stored Wi-Fi fix is country
 *    evidence only: it refreshes the fix's age and never moves it or opens
 *    a change candidate.
 *  - within a zone, a new estimate moves the fix (and with it the solar
 *    position) only beyond max(1 km, its own uncertainty), or when it is at
 *    least twice as precise -- IP noise never re-anchors sunrise/sunset, and
 *    a coarse IP answer never displaces a nearby precise Wi-Fi fix.
 *  - `loc_live off` ignores the live fix entirely (configured / none).
 *  - the accepted fix persists (NVS) on a zone change, a real move or a new
 *    agreeing estimate (the confirmation count must survive a reboot), and
 *    is used while younger than 30 days (with an untrusted clock only a fix
 *    accepted during this boot is used). */
#define DEVICE_LOCATION_MAX_ACCURACY_M   200000.0
#define DEVICE_LOCATION_CONFIRMATIONS    3
#define DEVICE_LOCATION_CONFIRM_SPAN_S   (6 * 3600)
#define DEVICE_LOCATION_AGREE_COUNT      2
#define DEVICE_LOCATION_AGREE_SPAN_S     3600
#define DEVICE_LOCATION_MAX_AGE_S        (30 * 86400)
#define DEVICE_LOCATION_PERSIST_MOVE_M   1000.0
/* Any UTC earlier than this means the clock is not trusted (2024-01-01). */
#define DEVICE_LOCATION_TRUSTED_UTC      1704067200LL

/* The persisted fallback coordinates. False when unset/unreadable/(0,0). */
bool device_location_configured(device_location_t *out);
/* The stored live fix, if still usable at `now_utc` (whether or not it
 * currently wins). */
bool device_location_live(device_location_t *out, int64_t now_utc);
/* The rules above; `out->source` says which (NONE: lat/lon unset). */
device_location_source_t device_location_effective(device_location_t *out, int64_t now_utc);
/* "live" | "configured" | "none" */
const char *device_location_source_name(device_location_source_t source);
/* "wifi" | "ip" | NULL */
const char *device_location_live_source_name(device_live_source_t source);

typedef enum {
    DEVICE_LOCATION_OFFER_ACCEPTED = 0,   /* stored as the live fix */
    DEVICE_LOCATION_OFFER_ZONE_CHANGE,    /* confirmed change of a live-derived zone */
    DEVICE_LOCATION_OFFER_CANDIDATE,      /* would change a live zone; not yet confirmed */
    DEVICE_LOCATION_OFFER_INACCURATE,     /* accuracy missing or worse than 200 km */
    DEVICE_LOCATION_OFFER_NO_ZONE,        /* resolves to no zone (open water) */
    DEVICE_LOCATION_OFFER_PERSIST_FAILED, /* NVS write failed: not adopted */
} device_location_offer_t;

/* Offer one live estimate (already validated by its source). Worker task
 * only. The NVS write happens outside the state lock, so readers on other
 * tasks (identity, STATUS, the MQTT task) never wait on flash. */
device_location_offer_t device_location_offer_live(double lat, double lon, double accuracy_m,
                                                   int64_t fix_utc, int64_t now_utc,
                                                   device_live_source_t source);
const char *device_location_offer_name(device_location_offer_t offer);

/* True once when the stored live fix has just aged out (or the clock became
 * trusted and showed it too old); sets live status "expired". */
bool device_location_check_expiry(int64_t now_utc);

/* Last live-source outcome for telemetry ("off", "ok", "no_fix",
 * "unreachable", "candidate", "expired", ...). Copied; at most 23 chars.
 * Its own short lock: safe from the esp-mqtt task. */
void device_location_set_live_status(const char *status);
void device_location_live_status(char *out, size_t len);

/* Host tests only: forget RAM state (as a reboot does). */
void device_location_reset_for_test(void);

#ifdef __cplusplus
}
#endif
