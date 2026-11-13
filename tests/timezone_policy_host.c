/* SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Timezone-from-location policy, end to end on the host: the production
 * timezone_command.c (worker + replies), device_location.c (country rule,
 * hysteresis, persistence), device_config.c (real NVS encoding over a fake
 * in-memory NVS that counts writes), timezone.c (libc rules), the generated
 * map (tz_geo.c over the checked-in tz_geo.bin) and pinned-IDF cJSON.
 * Replaced: NVS flash, the worker queue, the clock (--wrap=time).
 *
 * Usage: timezone_policy_host <tz_geo.bin> <scenario>; prints the last reply.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "device_config.h"
#include "device_location.h"
#include "nvs.h"
#include "timezone.h"
#include "timezone_command.h"
#include "time_sync.h"
#include "tz_geo.h"

/* ── fake NVS ─────────────────────────────────────────────────────────── */
typedef struct { char key[16]; uint8_t val[128]; size_t len; bool is_str; bool used; } entry_t;
static entry_t nvs[32];
static int writes;
static char fail_key[16];

static entry_t *find(const char *key)
{
    for (int i = 0; i < 32; i++) if (nvs[i].used && !strcmp(nvs[i].key, key)) return &nvs[i];
    return NULL;
}
static esp_err_t put(const char *key, const void *v, size_t len, bool is_str)
{
    if (fail_key[0] && !strcmp(fail_key, key)) return ESP_FAIL;
    entry_t *e = find(key);
    for (int i = 0; !e && i < 32; i++) if (!nvs[i].used) e = &nvs[i];
    assert(e && len <= sizeof e->val);
    snprintf(e->key, sizeof e->key, "%s", key);
    memcpy(e->val, v, len);
    e->len = len;
    e->is_str = is_str;
    e->used = true;
    writes++;
    return ESP_OK;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) { (void)h; entry_t *e = find(key); if (e) e->used = false; return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v) { (void)h; return put(key, v, strlen(v) + 1, true); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len) { (void)h; return put(key, v, len, false); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    entry_t *e = find(key);
    if (!e || !e->is_str) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, e->val, e->len);
    *len = e->len;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    entry_t *e = find(key);
    if (!e || e->is_str) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, e->val, e->len);
    *len = e->len;
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out) { (void)h; (void)key; (void)out; return ESP_ERR_NVS_NOT_FOUND; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }

/* ── clock, map, worker ───────────────────────────────────────────────── */
static time_t fake_now = 1794560000;   /* 2026-11-13 */
time_t __wrap_time(time_t *t) { if (t) *t = fake_now; return fake_now; }

static uint8_t *map;
static size_t map_len;
const char *tz_geo_zone_for(double lat, double lon) { return tz_geo_lookup(map, map_len, lat, lon); }

static char last[2048];
static void (*job)(void *);
static void *job_arg;
static void reply(const char *json) { assert(strlen(json) < sizeof last); strcpy(last, json); }
static bool submit(void (*run)(void *), void *arg) { assert(!job); job = run; job_arg = arg; return true; }
static int gate_held;
static bool begin(void) { gate_held++; return true; }
static void end(void) { gate_held--; }
static void run_job(void) { assert(job); void (*r)(void *) = job; job = NULL; r(job_arg); assert(!gate_held); }

static void boot(void)
{
    /* What app_main does: device_config_init, the boot apply, the reconcile. */
    device_location_reset_for_test();
    assert(device_config_init() == ESP_OK);
    char tz[48];
    if (device_config_get_timezone(tz, sizeof tz) != ESP_OK) tz[0] = '\0';
    timezone_apply(tz);
    timezone_command_reconcile_boot();
}
static void command(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    assert(root && timezone_command_handle(root));
    cJSON_Delete(root);
    if (job) run_job();
}
static const char *set_location(const char *id, double lat, double lon)
{
    /* The MQTT set_location handler's sequence (command_router.c). */
    bool changed = timezone_command_location_differs(lat, lon);
    assert(device_config_set_location(lat, lon, NULL) == ESP_OK);
    const char *resolved;
    const char *action = timezone_command_location_stored(id, changed, &resolved);
    if (job) run_job();
    return action;
}
static void live(double lat, double lon, double acc)
{
    assert(timezone_command_offer_live(lat, lon, acc, fake_now));
    run_job();
}
static void expect_active(const char *zone, const char *source)
{
    timezone_status_t st;
    timezone_command_status(&st);
    if (strcmp(timezone_current_name(), zone) || strcmp(st.source, source)) {
        fprintf(stderr, "active=%s source=%s, want %s/%s\n", timezone_current_name(), st.source, zone, source);
        abort();
    }
    assert(zone[0] == '\0' || timezone_rule_active());
}
static void expect_position(double lat, double lon)
{
    double a, b;
    time_sync_get_location(&a, &b, NULL);
    if (a != lat || b != lon) {
        fprintf(stderr, "position %.6f,%.6f, want %.6f,%.6f\n", a, b, lat, lon);
        abort();
    }
}
static void refresh(void) { timezone_command_refresh(); if (job) run_job(); }
static void seed_zone(const char *zone) { assert(device_config_init() == ESP_OK); assert(device_config_set_timezone(zone) == ESP_OK); }

#define ULURU       -25.3444, 131.0369
#define ADELAIDE_IP -34.8545, 138.6471   /* AWS IP solver on Uluru's cellular egress, +-50 km */
#define OFFICE_IP    52.3700,   4.8900   /* a Dutch IP-solver answer, +-5 km (synthetic point) */
#define AMSTERDAM    52.3676,   4.9041
#define OCEAN         0.0,    -30.0

int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    map_len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    map = malloc(map_len);
    assert(map && fread(map, 1, map_len, f) == map_len);
    fclose(f);
    timezone_command_config_t cfg = {.reply = reply, .submit = submit,
                                     .maintenance_begin = begin, .maintenance_end = end};
    timezone_command_init(&cfg);
    assert(device_config_init() == ESP_OK);
    const char *s = argv[2];

    if (!strcmp(s, "boot_without_location")) {
        seed_zone("Europe/Amsterdam");
        writes = 0;
        boot();
        expect_active("Europe/Amsterdam", "configured");
        assert(writes == 0);
    } else if (!strcmp(s, "upgrade_from_2_16_berlin")) {
        /* 2.16.0 state: provisioning stamped Berlin, Uluru coordinates, no tz_source. */
        seed_zone("Europe/Berlin");
        assert(device_config_set_location(ULURU, NULL) == ESP_OK);
        writes = 0;
        boot();
        expect_active("Australia/Darwin", "location");
        assert(writes == 0);   /* derived, not persisted: the fallback stays Berlin */
        char tz[48];
        assert(device_config_get_timezone(tz, sizeof tz) == ESP_OK && !strcmp(tz, "Europe/Berlin"));
        assert(timezone_utc_offset_seconds(fake_now) == 34200);
    } else if (!strcmp(s, "upgrade_from_2_16_darwin")) {
        /* The five Uluru units after the #98 rollout: Darwin + Uluru coordinates. */
        seed_zone("Australia/Darwin");
        assert(device_config_set_location(ULURU, NULL) == ESP_OK);
        writes = 0;
        boot();
        boot();
        expect_active("Australia/Darwin", "location");
        assert(writes == 0);
    } else if (!strcmp(s, "set_location_applies")) {
        seed_zone("Europe/Berlin");
        boot();
        expect_active("Europe/Berlin", "configured");
        const char *action = set_location("loc-1", ULURU);
        assert(!strcmp(action, "queued"));
        expect_active("Australia/Darwin", "location");
        assert(strstr(last, "\"type\":\"timezone_result\"") && strstr(last, "\"trigger\":\"set_location\""));
        assert(strstr(last, "\"active_timezone\":\"Australia/Darwin\"") && strstr(last, "\"applied\":true"));
        /* A retained re-send of the same coordinates changes nothing. */
        writes = 0;
        assert(!strcmp(set_location("loc-2", ULURU), "unchanged"));
        assert(writes == 1);   /* the site_state blob itself (command_router persists it) */
    } else if (!strcmp(s, "plain_set_is_a_fallback")) {
        seed_zone("Europe/Berlin");
        assert(device_config_set_location(ULURU, NULL) == ESP_OK);
        boot();
        command("{\"type\":\"set_timezone\",\"id\":\"t1\",\"timezone\":\"Asia/Tokyo\"}");
        expect_active("Australia/Darwin", "location");
        assert(strstr(last, "\"timezone\":\"Asia/Tokyo\"") && strstr(last, "\"requested_timezone\":\"Asia/Tokyo\""));
        assert(strstr(last, "\"active_timezone\":\"Australia/Darwin\"") && strstr(last, "\"timezone_source\":\"location\""));
        assert(strstr(last, "\"ok\":true") && strstr(last, "\"pinned\":false"));
        writes = 0;
        command("{\"type\":\"set_timezone\",\"id\":\"t2\",\"timezone\":\"Asia/Tokyo\"}");
        assert(writes == 0);   /* desired state already stored */
    } else if (!strcmp(s, "pin_wins_until_location_changes")) {
        seed_zone("Europe/Berlin");
        assert(device_config_set_location(ULURU, NULL) == ESP_OK);
        boot();
        command("{\"type\":\"set_timezone\",\"id\":\"p1\",\"timezone\":\"Europe/Berlin\",\"pin\":true}");
        expect_active("Europe/Berlin", "manual");
        assert(strstr(last, "\"pinned\":true") && strstr(last, "\"applied\":true"));
        boot();
        expect_active("Europe/Berlin", "manual");
        assert(!strcmp(set_location("same", ULURU), "pinned"));
        expect_active("Europe/Berlin", "manual");
        live(ADELAIDE_IP, 50000);              /* a pin beats a live estimate too */
        expect_active("Europe/Berlin", "manual");
        assert(!strcmp(set_location("moved", -34.9285, 138.6007), "queued"));
        expect_active("Australia/Adelaide", "location");
        assert(device_config_timezone_source() == DEVICE_TZ_SOURCE_AUTO);
    } else if (!strcmp(s, "auto_releases_pin")) {
        assert(device_config_set_location(ULURU, NULL) == ESP_OK);
        boot();
        command("{\"type\":\"set_timezone\",\"id\":\"p\",\"timezone\":\"Europe/Berlin\",\"pin\":true}");
        expect_active("Europe/Berlin", "manual");
        command("{\"type\":\"set_timezone\",\"id\":\"a\",\"timezone\":\"auto\"}");
        expect_active("Australia/Darwin", "location");
        assert(strstr(last, "set_timezone_result") && strstr(last, "\"pinned\":false"));
        command("{\"type\":\"set_timezone\",\"id\":\"bad\",\"timezone\":\"Europe/Berlin\",\"pin\":\"yes\"}");
        assert(strstr(last, "invalid_pin"));
    } else if (!strcmp(s, "live_beats_preset_from_another_country")) {
        /* Preset in Amsterdam (the GUI laptop's site), deployed at Uluru. */
        seed_zone("Europe/Amsterdam");
        assert(device_config_set_location(AMSTERDAM, NULL) == ESP_OK);
        boot();
        expect_active("Europe/Amsterdam", "location");
        live(ADELAIDE_IP, 50000);
        expect_active("Australia/Adelaide", "location");
        timezone_status_t st;
        timezone_command_status(&st);
        assert(st.location.source == DEVICE_LOCATION_LIVE && st.location.configured_stale);
    } else if (!strcmp(s, "configured_beats_live_in_same_country")) {
        seed_zone("Europe/Berlin");
        assert(device_config_set_location(ULURU, NULL) == ESP_OK);
        boot();
        live(ADELAIDE_IP, 50000);
        expect_active("Australia/Darwin", "location");
        timezone_status_t st;
        timezone_command_status(&st);
        assert(st.location.source == DEVICE_LOCATION_CONFIGURED && !st.location.configured_stale);
        assert(st.live_set && fabs(st.live.lat - -34.8545) < 1e-9);   /* informational */
    } else if (!strcmp(s, "live_without_configured_applies_at_once")) {
        seed_zone("Europe/Berlin");
        boot();
        live(ADELAIDE_IP, 50000);
        expect_active("Australia/Adelaide", "location");
        writes = 0;
        live(ADELAIDE_IP, 50000);              /* same place: no flash write */
        assert(writes == 0);
        /* Persisted: a reboot without uplink keeps the live location... */
        boot();
        expect_active("Australia/Adelaide", "location");
        /* ...for 30 days. */
        fake_now += 31 * 86400;
        boot();
        expect_active("Europe/Berlin", "configured");
    } else if (!strcmp(s, "office_ip_estimate")) {
        boot();
        live(OFFICE_IP, 5000);
        expect_active("Europe/Amsterdam", "location");
    } else if (!strcmp(s, "live_zone_change_needs_confirmation")) {
        boot();
        live(ADELAIDE_IP, 50000);
        expect_active("Australia/Adelaide", "location");
        /* Now the zone is live-derived: a different zone needs 3 agreeing
         * estimates spanning >= 6 h. */
        live(ULURU, 50000);
        assert(device_location_live(NULL, fake_now));
        expect_active("Australia/Adelaide", "location");
        fake_now += 3 * 3600;
        live(ULURU, 50000);
        expect_active("Australia/Adelaide", "location");
        fake_now += 3 * 3600;
        live(ULURU, 50000);
        expect_active("Australia/Darwin", "location");
        /* A disagreeing estimate restarts the count. */
        live(ADELAIDE_IP, 50000);
        fake_now += 7 * 3600;
        live(OFFICE_IP, 5000);
        live(OFFICE_IP, 5000);
        expect_active("Australia/Darwin", "location");
    } else if (!strcmp(s, "unusable_estimates_ignored")) {
        seed_zone("Europe/Berlin");
        boot();
        writes = 0;
        live(OCEAN, 5000);
        live(ADELAIDE_IP, 250000);             /* beyond the 200 km sanity floor */
        live(ADELAIDE_IP, NAN);                /* accuracy missing */
        live(0.0, 0.0, 10);
        expect_active("Europe/Berlin", "configured");
        assert(writes == 0 && !device_location_live(NULL, fake_now));
    } else if (!strcmp(s, "persist_failure_reports_and_keeps_zone")) {
        seed_zone("Europe/Berlin");
        boot();
        strcpy(fail_key, "timezone");
        command("{\"type\":\"set_timezone\",\"id\":\"f\",\"timezone\":\"Asia/Tokyo\"}");
        assert(strstr(last, "persist_failed") && strstr(last, "\"ok\":false"));
        expect_active("Europe/Berlin", "configured");
        fail_key[0] = '\0';
        strcpy(fail_key, "loc_live_fix");
        live(ADELAIDE_IP, 50000);
        expect_active("Europe/Berlin", "configured");
    } else if (!strcmp(s, "solar_follows_live")) {
        /* The Uluru failure mode: preset with no configured coordinates. The
         * accepted live fix must drive the sun position, not just the zone. */
        seed_zone("Europe/Berlin");
        boot();
        expect_position(TIME_SYNC_DEFAULT_LAT, TIME_SYNC_DEFAULT_LON);
        timezone_status_t st;
        timezone_command_status(&st);
        assert(!strcmp(st.solar_position, "default") && st.location.source == DEVICE_LOCATION_NONE);
        live(ULURU, 50000);
        expect_active("Australia/Darwin", "location");
        expect_position(-25.3444, 131.0369);
        timezone_command_status(&st);
        assert(!strcmp(st.solar_position, "location"));
        /* 2026-11-13 00:30 UTC = 10:00 in Darwin: daytime at Uluru, night in
         * the Netherlands; sunrise lands in the Uluru dawn. */
        int64_t utc = 1794529800;
        int64_t local = utc + timezone_utc_offset_seconds(utc);
        assert(time_sync_is_daytime(local));
        int64_t sunrise;
        assert(time_sync_sun_on_date(local, TIME_SYNC_SUNRISE, &sunrise) == ESP_OK);
        int h, m;
        time_sync_localtime(sunrise, NULL, NULL, NULL, &h, &m, NULL, NULL);
        assert(h == 6 || h == 5);   /* ~05:5x Darwin time in mid November */
        time_sync_set_position(TIME_SYNC_DEFAULT_LAT, TIME_SYNC_DEFAULT_LON);
        assert(!time_sync_is_daytime(utc + 3600));   /* 01:30 CET in NL: night */
    } else if (!strcmp(s, "solar_ignores_live_noise")) {
        boot();
        live(-34.9285, 138.6007, 5000);                 /* IP-level fix in Adelaide */
        expect_position(-34.9285, 138.6007);
        uint32_t gen = time_sync_position_generation();
        live(-34.9285 + 0.027, 138.6007, 5000);         /* ~3 km, inside +-5 km */
        expect_position(-34.9285, 138.6007);
        assert(time_sync_position_generation() == gen);
        live(-34.9285 + 0.027, 138.6007, 41);           /* Wi-Fi: 2x sharper, moves */
        expect_position(-34.9285 + 0.027, 138.6007);
        gen = time_sync_position_generation();
        live(-34.9285 + 0.0315, 138.6007, 41);          /* ~500 m: under 1 km, stays */
        expect_position(-34.9285 + 0.027, 138.6007);
        assert(time_sync_position_generation() == gen);
    } else if (!strcmp(s, "solar_configured_and_default")) {
        assert(device_config_set_location(ULURU, NULL) == ESP_OK);
        boot();
        expect_position(-25.3444, 131.0369);
        /* configured Uluru + live Adelaide: same country, configured wins for
         * the zone AND the sun position. */
        live(ADELAIDE_IP, 50000);
        expect_position(-25.3444, 131.0369);
        timezone_status_t st;
        timezone_command_status(&st);
        assert(!strcmp(st.solar_position, "location"));
    } else if (!strcmp(s, "loc_live_off_drops_live")) {
        seed_zone("Europe/Berlin");
        boot();
        live(ADELAIDE_IP, 50000);
        expect_active("Australia/Adelaide", "location");
        expect_position(-34.8545, 138.6471);
        assert(device_config_set_live_location_mode("off") == ESP_OK);
        refresh();
        expect_active("Europe/Berlin", "configured");
        expect_position(TIME_SYNC_DEFAULT_LAT, TIME_SYNC_DEFAULT_LON);
        assert(device_config_set_live_location_mode("aws") == ESP_OK);
        refresh();
        expect_active("Australia/Adelaide", "location");
        /* Expiry without any new input: the periodic refresh catches it. */
        fake_now += 31 * 86400;
        refresh();
        expect_active("Europe/Berlin", "configured");
        expect_position(TIME_SYNC_DEFAULT_LAT, TIME_SYNC_DEFAULT_LON);
    } else if (!strcmp(s, "pin_keeps_solar_location")) {
        assert(device_config_set_location(ULURU, NULL) == ESP_OK);
        boot();
        command("{\"type\":\"set_timezone\",\"id\":\"p\",\"timezone\":\"Europe/Berlin\",\"pin\":true}");
        expect_active("Europe/Berlin", "manual");
        expect_position(-25.3444, 131.0369);   /* the pin holds the zone only */
        /* set_location applies the position at once even when the zone is pinned. */
        assert(!strcmp(set_location("same", ULURU), "pinned"));
        expect_position(-25.3444, 131.0369);
    } else {
        return 2;
    }
    puts(last[0] ? last : "{}");
    return 0;
}
