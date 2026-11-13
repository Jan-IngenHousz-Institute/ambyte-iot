/*
 * timezone_command.c — the one place the device's timezone is decided and
 * changed at runtime (docs/timezone-from-location.md, docs/remote-timezone.md).
 *
 * Model. Everything an operator or tool sets is a FALLBACK; the zone in effect
 * is derived:
 *   pinned (tz_source=manual)   -> the configured zone (explicit escape hatch)
 *   else effective location     -> its zone, from the on-device map
 *        (live > configured coordinates)
 *   else                        -> the configured zone (NVS "timezone")
 *   else                        -> none: time_sync's fallback offset
 * Only operator inputs are persisted (configured zone, pin, coordinates, the
 * accepted live fix); the derived zone is recomputed at boot and after every
 * input change, so a reboot reaches the same answer without writing flash.
 *
 * Every change runs on the resident maintenance worker under the shared
 * maintenance gate: persist only what differs, read it back, then apply the
 * derived zone to libc (verified, timezone_apply). Nothing here touches the
 * RTC, UTC timestamps, stored records, topics or the schedule file; the
 * scheduler re-anchors on its next poll.
 */
#include "timezone_command.h"
#include "device_config.h"
#include "device_location.h"
#include "esp_log.h"
#include "nvs.h"
#include "time_sync.h"
#include "timezone.h"
#include "tz_geo.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "tz_cmd"

static timezone_command_config_t s_config;
static atomic_bool s_busy;

typedef enum {
    REQ_GET,        /* get_timezone: report only */
    REQ_SET,        /* set_timezone <zone> [pin]: configured fallback (or pin) */
    REQ_AUTO,       /* set_timezone "auto": release a pin */
    REQ_LOCATION,   /* set_location / sync loc stored coordinates */
    REQ_BOOT,       /* boot reconciliation */
    REQ_LIVE,       /* a live location estimate arrived */
    REQ_REFRESH,    /* periodic re-evaluation (live fix expiry, loc_live off) */
} req_kind_t;

typedef struct {
    char id[65];
    char zone[48];
    req_kind_t kind;
    bool pin;
    bool location_changed;
    double lat, lon, accuracy_m;
    int64_t fix_utc;
    device_live_source_t source;
} request_t;

typedef enum { WHY_NONE, WHY_MANUAL, WHY_LOCATION, WHY_CONFIGURED } why_t;

static const char *why_name(why_t why)
{
    switch (why) {
    case WHY_MANUAL:     return "manual";
    case WHY_LOCATION:   return "location";
    case WHY_CONFIGURED: return "configured";
    default:             return "none";
    }
}

static int64_t now_utc(void)
{
    return (int64_t)time(NULL);
}

static bool pinned(void)
{
    device_tz_source_t s = device_config_timezone_source();
    /* An unreadable flag is treated as a pin: never override what cannot be read. */
    return s == DEVICE_TZ_SOURCE_MANUAL || s == DEVICE_TZ_SOURCE_UNKNOWN;
}

/* Zone of a coordinate, only if this firmware can apply it. */
static const char *zone_at(double lat, double lon)
{
    const char *zone = tz_geo_zone_for(lat, lon);
    char canonical[48];
    if (zone == NULL || timezone_canonicalize(zone, canonical, sizeof canonical) != ESP_OK ||
        strcmp(canonical, zone) != 0) {
        return NULL;
    }
    return zone;
}

typedef struct {
    why_t why;
    char zone[48];             /* the zone in effect ("" = none) */
    char configured[48];       /* NVS fallback zone ("" = unset) */
    bool pinned;
    device_location_t loc;     /* effective location */
    const char *resolved;      /* zone of the effective location, or NULL */
} decision_t;

/* `ignore_pin`: the zone the unpinned policy would choose (live offers). */
static void decide(decision_t *d, bool ignore_pin)
{
    memset(d, 0, sizeof *d);
    if (device_config_get_timezone(d->configured, sizeof d->configured) != ESP_OK) d->configured[0] = '\0';
    d->pinned = pinned();
    device_location_effective(&d->loc, now_utc());
    d->resolved = d->loc.source != DEVICE_LOCATION_NONE ? zone_at(d->loc.lat, d->loc.lon) : NULL;
    if (d->pinned && !ignore_pin) {
        d->why = d->configured[0] ? WHY_MANUAL : WHY_NONE;
        strcpy(d->zone, d->configured);
    } else if (d->resolved) {
        d->why = WHY_LOCATION;
        snprintf(d->zone, sizeof d->zone, "%s", d->resolved);
    } else if (d->configured[0]) {
        d->why = WHY_CONFIGURED;
        strcpy(d->zone, d->configured);
    } else {
        d->why = WHY_NONE;
    }
}

/* The sun-math position follows the SAME effective location as the zone
 * (one decision, one source): sunrise, sunset, solar triggers and day/night
 * windows depend on where the device is, and two sites in one zone can be in
 * different seasons. With no location the compiled default stays, reported
 * as solar_position "default". A pin holds the zone only; the position still
 * follows the location. Changing it bumps time_sync's position generation,
 * which makes the schedule re-resolve armed solar dues (sched_due). Live
 * noise never reaches here: device_location only moves the live fix beyond
 * its uncertainty. Returns true if the position changed. */
static bool apply_position(const decision_t *d)
{
    double lat = TIME_SYNC_DEFAULT_LAT, lon = TIME_SYNC_DEFAULT_LON;
    if (d->loc.source != DEVICE_LOCATION_NONE) {
        lat = d->loc.lat;
        lon = d->loc.lon;
    }
    double cur_lat, cur_lon;
    time_sync_get_location(&cur_lat, &cur_lon, NULL);
    if (cur_lat == lat && cur_lon == lon) return false;
    time_sync_set_position(lat, lon);
    ESP_LOGW(TAG, "solar position now from %s (%s)", device_location_source_name(d->loc.source),
             d->loc.source != DEVICE_LOCATION_NONE ? "location" : "default");
    return true;
}

/* Apply the derived zone if libc is not already running it, verified. */
static bool reconcile(decision_t *d)
{
    decide(d, false);
    (void)apply_position(d);
    const char *active = timezone_current_name();
    bool running = strcmp(active, d->zone) == 0 && (d->zone[0] == '\0' || timezone_rule_active());
    if (!running) {
        esp_err_t err = timezone_apply(d->zone[0] ? d->zone : NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "apply '%s' failed: %s", d->zone, esp_err_to_name(err));
        }
    }
    return strcmp(timezone_current_name(), d->zone) == 0 &&
           (d->zone[0] == '\0' || timezone_rule_active());
}

static void add_location(cJSON *reply, const decision_t *d)
{
    cJSON_AddStringToObject(reply, "timezone_source", why_name(d->why));
    cJSON_AddBoolToObject(reply, "pinned", d->pinned);
    cJSON_AddStringToObject(reply, "resolved_timezone", d->resolved ? d->resolved : "");
    cJSON_AddStringToObject(reply, "location_source", device_location_source_name(d->loc.source));
    cJSON_AddBoolToObject(reply, "location_set", d->loc.source != DEVICE_LOCATION_NONE);
}

static void report(const request_t *request, bool ok, bool saved, const char *detail)
{
    /* Read back every fact. A persisted value alone is never reported as the
     * active timezone: a failed write/readback leaves the runtime untouched. */
    decision_t d;
    decide(&d, false);
    const char *active;
    int32_t offset;
    timezone_snapshot(now_utc(), &active, &offset);
    /* applied: libc runs exactly the zone the policy derives, verified. */
    bool applied = strcmp(active, d.zone) == 0 && (d.zone[0] == '\0' || timezone_rule_active()) &&
                   d.zone[0] != '\0';

    if (request->kind == REQ_REFRESH) return;   /* apply_position/timezone log changes */
    if (request->kind == REQ_BOOT || request->kind == REQ_LIVE ||
        (request->kind == REQ_LOCATION && !request->id[0])) {
        /* No MQTT requester (boot, live estimate, USB CLI): the log is the record. */
        ESP_LOGW(TAG, "timezone %s (ok=%d saved=%d): active=%s source=%s pinned=%d "
                      "configured=%s location=%s resolved=%s offset=%ld",
                 detail, ok, saved, active[0] ? active : "-", why_name(d.why), d.pinned,
                 d.configured[0] ? d.configured : "-", device_location_source_name(d.loc.source),
                 d.resolved ? d.resolved : "-", (long)offset);
        return;
    }
    cJSON *reply = cJSON_CreateObject();
    if (!reply) return;
    const char *type = (request->kind == REQ_SET || request->kind == REQ_AUTO)
                       ? "set_timezone_result" : "timezone_result";
    cJSON_AddStringToObject(reply, "type", type);
    cJSON_AddStringToObject(reply, "id", request->id);
    if (request->kind == REQ_LOCATION) cJSON_AddStringToObject(reply, "trigger", "set_location");
    cJSON_AddBoolToObject(reply, "ok", ok);
    cJSON_AddBoolToObject(reply, "saved", saved);
    /* #98 field names. `timezone` is the CONFIGURED (fallback) zone;
     * `active_timezone` the zone in effect. They differ when the location wins. */
    cJSON_AddStringToObject(reply, "timezone", d.configured);
    cJSON_AddStringToObject(reply, "active_timezone", active);
    cJSON_AddBoolToObject(reply, "applied", applied && ok);
    cJSON_AddNumberToObject(reply, "utc_offset_seconds", offset);
    cJSON_AddStringToObject(reply, "detail", detail);
    if (request->kind == REQ_SET) {
        cJSON_AddStringToObject(reply, "requested_timezone", request->zone);
        cJSON_AddBoolToObject(reply, "requested_pin", request->pin);
    }
    add_location(reply, &d);
    char *json = cJSON_PrintUnformatted(reply);
    if (json) {
        if (s_config.reply) s_config.reply(json);
        cJSON_free(json);
    }
    cJSON_Delete(reply);
}

typedef struct {
    bool ok;
    bool saved;
    const char *detail;
} outcome_t;

/* Persist the configured zone (and pin) only where it differs, read back. A
 * pin writes "manual" BEFORE the zone: a power cut then leaves the old zone
 * pinned, never the new zone unpinned and silently overridden. */
static outcome_t persist_zone(const char *zone, bool pin)
{
    char stored[48] = "";
    esp_err_t err = device_config_get_timezone(stored, sizeof stored);
    /* Unknown reads refuse rather than guessing whether a previous write
     * persisted. NOT_FOUND (unset) and INVALID_ARG (unsupported stored name)
     * are repairable by writing. */
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_INVALID_ARG) {
        return (outcome_t){false, false, "read_failed"};
    }
    if (pin && device_config_timezone_source() != DEVICE_TZ_SOURCE_MANUAL &&
        device_config_set_timezone_source("manual") != ESP_OK) {
        return (outcome_t){false, false, "persist_failed"};
    }
    /* Desired-state idempotency: QoS 1 duplicates and retries cost no write. */
    if (!(err == ESP_OK && strcmp(stored, zone) == 0) && device_config_set_timezone(zone) != ESP_OK) {
        return (outcome_t){false, false, "persist_failed"};
    }
    if (device_config_get_timezone(stored, sizeof stored) != ESP_OK || strcmp(stored, zone) != 0 ||
        (pin && device_config_timezone_source() != DEVICE_TZ_SOURCE_MANUAL)) {
        return (outcome_t){false, false, "readback_failed"};
    }
    return (outcome_t){true, true, "applied"};
}

/* Release a pin ("auto"). An absent flag already means unpinned: no write. */
static bool unpin(void)
{
    device_tz_source_t s = device_config_timezone_source();
    if (s == DEVICE_TZ_SOURCE_AUTO || s == DEVICE_TZ_SOURCE_PROVISIONED) return true;
    return device_config_set_timezone_source("auto") == ESP_OK &&
           device_config_timezone_source() == DEVICE_TZ_SOURCE_AUTO;
}

static void execute(void *arg)
{
    request_t *request = arg;
    if (request->kind == REQ_GET) {
        report(request, true, false, "read_only");
        goto done;
    }
    bool gated = s_config.maintenance_begin != NULL && s_config.maintenance_end != NULL;
    if (gated && !s_config.maintenance_begin()) {
        report(request, false, false, "busy");
        goto done;
    }
    outcome_t out = {true, false, "applied"};
    decision_t d;
    switch (request->kind) {
    case REQ_SET:
        out = persist_zone(request->zone, request->pin);
        break;
    case REQ_AUTO:
        if (!unpin()) out = (outcome_t){false, false, "persist_failed"};
        else out.saved = true;
        break;
    case REQ_LOCATION:
        /* A changed location releases a pin; an identical re-send (retained
         * set_location replay) does not. */
        if (request->location_changed && pinned() && !unpin()) {
            out = (outcome_t){false, false, "persist_failed"};
        }
        break;
    case REQ_LIVE: {
        device_location_offer_t offer = device_location_offer_live(
            request->lat, request->lon, request->accuracy_m, request->fix_utc, now_utc(),
            request->source);
        device_location_set_live_status(device_location_offer_name(offer));
        out.detail = device_location_offer_name(offer);
        if (offer == DEVICE_LOCATION_OFFER_PERSIST_FAILED) out.ok = false;
        break;
    }
    case REQ_REFRESH:
        /* A live fix past its 30 days (or shown too old once the clock is
         * trusted) is reported and dropped by the reconcile below. */
        (void)device_location_check_expiry(now_utc());
        break;
    case REQ_BOOT:
    default:
        break;
    }
    if (out.ok) {
        bool applied = reconcile(&d);
        if (!applied && d.zone[0] != '\0') out = (outcome_t){false, out.saved, "apply_failed"};
    }
    report(request, out.ok, out.saved, out.detail);
    if (gated) s_config.maintenance_end();
done:
    free(request);
    atomic_store(&s_busy, false);
}

/* Single-flight hand-off to the resident worker. Returns NULL when queued,
 * else the refusal detail ("busy" / "queue_unavailable"). */
static const char *dispatch(const request_t *request)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) return "busy";
    request_t *copy = malloc(sizeof *copy);
    if (copy) *copy = *request;
    if (!copy || !s_config.submit || !s_config.submit(execute, copy)) {
        free(copy);
        atomic_store(&s_busy, false);
        return "queue_unavailable";
    }
    return NULL;
}

void timezone_command_init(const timezone_command_config_t *config)
{
    s_config = *config;
}

bool timezone_command_handle(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) return false;
    bool write = strcmp(type->valuestring, "set_timezone") == 0;
    if (!write && strcmp(type->valuestring, "get_timezone") != 0) return false;
    request_t request = {.kind = write ? REQ_SET : REQ_GET};
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) >= sizeof request.id) {
        report(&request, false, false, "invalid_id");
        return true;
    }
    memcpy(request.id, id->valuestring, strlen(id->valuestring) + 1);
    const cJSON *zone = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(root, "pin");
    if (write && pin != NULL && !cJSON_IsBool(pin)) {
        report(&request, false, false, "invalid_pin");
        return true;
    }
    request.pin = cJSON_IsTrue(pin);
    if (write && cJSON_IsString(zone) && strcmp(zone->valuestring, "auto") == 0) {
        request.kind = REQ_AUTO;
    } else if (write && (!cJSON_IsString(zone) ||
                         timezone_canonicalize(zone->valuestring, request.zone, sizeof request.zone) != ESP_OK ||
                         !request.zone[0])) {
        report(&request, false, false, "invalid_timezone");
        return true;
    }
    const char *refused = dispatch(&request);
    if (refused) report(&request, false, false, refused);
    return true;
}

bool timezone_command_location_differs(double lat, double lon)
{
    double old_lat, old_lon;
    /* Exact comparison on purpose: the stored doubles round-trip bit-exactly,
     * so only a re-send of the very same coordinates counts as "unchanged". */
    return device_config_get_location(&old_lat, &old_lon, NULL, 0) != ESP_OK ||
           old_lat != lat || old_lon != lon;
}

static void fill_status(const decision_t *d, timezone_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->location = d->loc;
    snprintf(out->configured, sizeof out->configured, "%s", d->configured);
    snprintf(out->resolved, sizeof out->resolved, "%s", d->resolved ? d->resolved : "");
    out->source = why_name(d->why);
    out->pinned = d->pinned;
    out->solar_position = d->loc.source != DEVICE_LOCATION_NONE ? "location" : "default";
    device_location_t configured;
    out->configured_set = device_location_configured(&configured);
    out->configured_lat = configured.lat;
    out->configured_lon = configured.lon;
    device_location_t live;
    out->live_set = device_location_live(&live, now_utc());
    out->live = live;
    device_location_live_status(out->live_status, sizeof out->live_status);
}

const char *timezone_command_location_stored(const char *id, bool changed, timezone_status_t *status)
{
    /* One decision on the calling (MQTT / console) task: it drives the
     * position, the dispatch choice and the caller's reply. Reads only; the
     * device_location lock is never held across flash I/O. */
    decision_t d;
    decide(&d, false);
    /* The position takes effect now, even when the zone needs no worker
     * job (same zone, or pinned). */
    (void)apply_position(&d);
    if (status) fill_status(&d, status);
    bool will_unpin = changed && d.pinned;
    if (!will_unpin && strcmp(timezone_current_name(), d.zone) == 0 &&
        (d.zone[0] == '\0' || timezone_rule_active())) {
        /* Nothing to persist or apply. A pinned zone that the location does
         * not touch also lands here. */
        return d.pinned ? "pinned" : "unchanged";
    }
    request_t request = {.kind = REQ_LOCATION, .location_changed = changed};
    if (id) snprintf(request.id, sizeof request.id, "%s", id);
    const char *refused = dispatch(&request);
    return refused ? refused : "queued";
}

bool timezone_command_offer_live(double lat, double lon, double accuracy_m, int64_t fix_utc,
                                 device_live_source_t source)
{
    request_t request = {.kind = REQ_LIVE, .lat = lat, .lon = lon,
                         .accuracy_m = accuracy_m, .fix_utc = fix_utc, .source = source};
    const char *refused = dispatch(&request);
    if (refused) {
        /* The next estimate (every 6 h, or after the next connect) retries. */
        device_location_set_live_status(refused);
        ESP_LOGW(TAG, "live location estimate not processed: %s", refused);
        return false;
    }
    return true;
}

void timezone_command_refresh(void)
{
    request_t request = {.kind = REQ_REFRESH};
    (void)dispatch(&request);   /* busy: the next refresh or input retries */
}

void timezone_command_reconcile_boot(void)
{
    /* Runs once in app_main, after the boot timezone_apply and before the
     * schedule runner starts, so the derived zone is in place for the first
     * scheduling decision (no re-anchor at boot). The MQTT start is parked
     * behind the boot gate and the CLI is not up yet, so nothing else can
     * submit a timezone change; the single-flight flag and the maintenance
     * gate are still taken, exactly as on the worker. No NVS write. */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        ESP_LOGW(TAG, "timezone at boot not reconciled: busy");
        return;
    }
    request_t *copy = calloc(1, sizeof *copy);
    if (!copy) {
        atomic_store(&s_busy, false);
        ESP_LOGW(TAG, "timezone at boot not reconciled: no memory");
        return;
    }
    copy->kind = REQ_BOOT;
    execute(copy);   /* frees copy, clears s_busy */
}

void timezone_command_status(timezone_status_t *out)
{
    if (out == NULL) return;
    decision_t d;
    decide(&d, false);
    fill_status(&d, out);
}
