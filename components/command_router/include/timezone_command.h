#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "device_location.h"

/* The shared worker owns persistence + apply. MQTT copies bounded input and does no persistence or runtime apply.
 * Fast refusal replies may read the current NVS/runtime timezone snapshot. */
typedef struct {
    void (*reply)(const char *json);
    bool (*submit)(void (*run)(void *), void *arg);
    bool (*maintenance_begin)(void);
    void (*maintenance_end)(void);
} timezone_command_config_t;

void timezone_command_init(const timezone_command_config_t *config);
/* Returns false for other commands. Caller still owns root.
 * set_timezone {"timezone": "<IANA>"} sets the CONFIGURED fallback zone (in
 * effect only while no location resolves); add "pin": true to pin it over the
 * location (escape hatch); {"timezone": "auto"} releases a pin. */
bool timezone_command_handle(const cJSON *root);

/* Timezone from the deployment location (docs/timezone-from-location.md). */

/* Call BEFORE persisting new coordinates: true unless exactly these
 * coordinates are already stored. A changed location releases a pin. */
bool timezone_command_location_differs(double lat, double lon);

/* Call AFTER new coordinates were persisted (MQTT set_location: `id` is the
 * request id, the worker publishes a timezone_result with trigger
 * "set_location"; USB CLI: id NULL, logged only). Applies the sun position at
 * once and fills `*status` (may be NULL) from the same single decision, for
 * the caller's reply. Returns the immediate action:
 *   "queued"      the worker re-derives and applies the zone
 *   "unchanged"   the zone in effect already matches
 *   "pinned"      a pinned zone is kept (coordinates did not change)
 *   "busy" / "queue_unavailable"  not attempted now; the next boot reconciles */
typedef struct timezone_status timezone_status_t;
const char *timezone_command_location_stored(const char *id, bool changed, timezone_status_t *status);

/* A live location estimate from an online source (already validated by the
 * source), with how it was obtained (Wi-Fi scan or public IP: IP estimates
 * never override a configured location, device_location.h). Queued to the
 * worker; false if it could not be queued. */
bool timezone_command_offer_live(double lat, double lon, double accuracy_m, int64_t fix_utc,
                                 device_live_source_t source);

/* Periodic re-evaluation on the worker (the app calls it every 6 h): a live
 * fix ageing past 30 days or `loc_live off` changes the effective location
 * without any other input. Silent unless something changes. */
void timezone_command_refresh(void);

/* One synchronous reconciliation at boot (after the boot timezone_apply,
 * before the schedule runner starts). Writes nothing. */
void timezone_command_reconcile_boot(void);

/* Snapshot for identity / STATUS. */
struct timezone_status {
    device_location_t location;     /* effective */
    bool configured_set;
    double configured_lat, configured_lon;
    bool live_set;
    device_location_t live;
    char live_status[24];
    char configured[48];            /* configured fallback zone */
    char resolved[48];              /* zone of the effective location */
    const char *source;             /* why the active zone: manual|location|configured|none */
    bool pinned;
    const char *solar_position;     /* "location" (effective) | "default" (compiled NL) */
};
void timezone_command_status(timezone_status_t *out);
