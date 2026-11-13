#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CLI.h"
#include "cli_identity.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "sched_runner.h"
#include "script_update.h"
#include <math.h>
#include "timezone.h"
#include "timezone_command.h"

static esp_app_desc_t app = { .version = "2.6.0-a2" };
static const char *mac = "02:00:00:00:00:01";
static bool description_missing;
static sched_identity_t loaded;
static script_identity_t release;
static esp_err_t snapshot_error, release_error;
static bool reload, reload_same_sha;
static unsigned snapshot_calls, release_calls;
static atomic_bool race_enabled, attempted, blocked, done;

const esp_app_desc_t *esp_app_get_description(void)
{
    return description_missing ? NULL : &app;
}

const char *device_commands_get_mac(void) { return mac; }

/* Location / timezone provenance boundary (timezone_command_status). */
static timezone_status_t tz_state;
static const char *active_zone = "Europe/Amsterdam";
static int32_t active_offset = 3600;
void timezone_command_status(timezone_status_t *out) { *out = tz_state; }
const char *device_location_source_name(device_location_source_t s)
{
    return s == DEVICE_LOCATION_LIVE ? "live" : s == DEVICE_LOCATION_CONFIGURED ? "configured" : "none";
}
void timezone_snapshot(int64_t utc, const char **name, int32_t *offset)
{
    (void)utc;
    *name = active_zone;
    *offset = active_offset;
}
static void tz_defaults(void)
{
    memset(&tz_state, 0, sizeof tz_state);
    tz_state.location = (device_location_t){DEVICE_LOCATION_NONE, 0, 0, NAN, -1, false};
    tz_state.live = tz_state.location;
    strcpy(tz_state.configured, "Europe/Amsterdam");
    tz_state.source = "configured";
    strcpy(tz_state.live_status, "waiting");
    tz_state.solar_position = "default";
}
static void tz_configured_uluru(void)
{
    tz_state.configured_set = true;
    tz_state.configured_lat = -25.3444;
    tz_state.configured_lon = 131.0369;
    tz_state.location = (device_location_t){DEVICE_LOCATION_CONFIGURED, -25.3444, 131.0369, NAN, -1, false};
    tz_state.solar_position = "location";
    strcpy(tz_state.configured, "Europe/Berlin");
    strcpy(tz_state.resolved, "Australia/Darwin");
    tz_state.source = "location";
    active_zone = "Australia/Darwin";
    active_offset = 34200;
}
static void tz_live_adelaide(void)
{
    tz_state.live_set = true;
    tz_state.live = (device_location_t){DEVICE_LOCATION_LIVE, -34.8545, 138.6471, 50000, 120, false};
    strcpy(tz_state.live_status, "ok");
}
size_t heap_caps_get_free_size(unsigned caps) { assert(caps == MALLOC_CAP_INTERNAL); return 32768; }
size_t heap_caps_get_largest_free_block(unsigned caps) { assert(caps == MALLOC_CAP_INTERNAL); return 16384; }
size_t heap_caps_get_minimum_free_size(unsigned caps) { assert(caps == MALLOC_CAP_INTERNAL); return 30000; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { assert(task == NULL); return 8192; }

esp_err_t sched_runner_identity(sched_identity_t *out)
{
    snapshot_calls++;
    *out = loaded;
    return snapshot_error;
}

esp_err_t script_update_get_identity(script_identity_t *out)
{
    release_calls++;
    *out = release;
    if (reload || reload_same_sha) {
        loaded.generation++;
        if (reload) {
            memset(loaded.sha256, 'b', 64);
            strcpy(loaded.workbook, "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
        }
    }
    return release_error;
}

/* Called by the production fwrite while it owns stdout. A real competing
 * thread tries the lock here, so missing response locking makes this fail. */
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
size_t __wrap_fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    if (atomic_exchange(&race_enabled, false)) {
        while (!atomic_load(&attempted)) { }
        assert(atomic_load(&blocked));
    }
    return __real_fwrite(buf, size, n, f);
}

static void *logger(void *unused)
{
    (void)unused;
    /* Wait until the handler reaches its actual write, not just a synthetic
     * test-owned flockfile section. */
    while (atomic_load(&race_enabled)) { }
    int got = ftrylockfile(stdout);
    atomic_store(&blocked, got != 0);
    if (got == 0) funlockfile(stdout);
    atomic_store(&attempted, true);
    unsigned count = 0;
    do {
        flockfile(stdout);
        fprintf(stdout, "I (host) unrelated log %u\n", count++);
        fflush(stdout);
        funlockfile(stdout);
    } while (!atomic_load(&done) || count < 100);
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    const char *scenario = argv[1];
    loaded.kind = SCHED_SOURCE_INSTALLED;
    loaded.generation = 7;
    memset(loaded.sha256, 'a', 64);
    strcpy(loaded.workbook, "11111111-2222-4333-8444-555555555555");
    loaded.has_workbook = true;
    strcpy(release.sha256, loaded.sha256);
    strcpy(release.version, "schedule-v1.2.3");
    strcpy(release.built_against_fw, "2.5.0");
    strcpy(release.installed_on_fw, "2.5.0");
    release.release_metadata_verified = true;
    cli_set_thing_name("Field_MixedCase_7");
    tz_defaults();
    if (!strcmp(scenario, "utf8_bytes")) {
        assert(argc == 3 && strlen(argv[2]) <= 62 && strlen(argv[2]) % 2 == 0);
        static char bytes[32];
        for (size_t i = 0; i < strlen(argv[2]) / 2; ++i) {
            unsigned value;
            assert(sscanf(argv[2] + i * 2, "%2x", &value) == 1 && value != 0);
            bytes[i] = (char)value;
        }
        cli_set_thing_name(bytes);
        strcpy(app.version, bytes);
        strcpy(loaded.workbook, bytes);
        strcpy(release.version, bytes);
    } else if (!strcmp(scenario, "tz_configured")) {
        tz_configured_uluru();
    } else if (!strcmp(scenario, "tz_live_accepted")) {
        /* No configured location; the live estimate drives the zone. */
        tz_live_adelaide();
        tz_state.solar_position = "location";
        tz_state.location = tz_state.live;
        strcpy(tz_state.configured, "Europe/Berlin");
        strcpy(tz_state.resolved, "Australia/Adelaide");
        tz_state.source = "location";
        active_zone = "Australia/Adelaide";
        active_offset = 37800;
    } else if (!strcmp(scenario, "tz_live_rejected")) {
        tz_configured_uluru();
        strcpy(tz_state.live_status, "rejected_404");
    } else if (!strcmp(scenario, "tz_same_country")) {
        /* Configured Uluru wins over the in-country IP estimate (informational). */
        tz_configured_uluru();
        tz_live_adelaide();
    } else if (!strcmp(scenario, "tz_configured_stale")) {
        tz_live_adelaide();
        tz_state.solar_position = "location";
        tz_state.configured_set = true;
        tz_state.configured_lat = 52.3676;
        tz_state.configured_lon = 4.9041;
        tz_state.location = tz_state.live;
        tz_state.location.configured_stale = true;
        strcpy(tz_state.resolved, "Australia/Adelaide");
        tz_state.source = "location";
        active_zone = "Australia/Adelaide";
        active_offset = 37800;
    } else if (!strcmp(scenario, "tz_pinned")) {
        tz_configured_uluru();
        tz_state.pinned = true;
        tz_state.source = "manual";
        active_zone = "Europe/Berlin";
        active_offset = 3600;
    } else if (!strcmp(scenario, "tz_unset")) {
        tz_state.configured[0] = '\0';
        tz_state.source = "none";
        active_zone = "";
        active_offset = 7200;
    } else if (!strcmp(scenario, "none")) {
        memset(&loaded, 0, sizeof loaded);
    } else if (!strcmp(scenario, "template")) {
        cli_set_thing_name("ambyte_02:00:00:00:00:01");
    } else if (!strcmp(scenario, "default")) {
        loaded.kind = SCHED_SOURCE_EMBEDDED_DEFAULT;
    } else if (!strcmp(scenario, "fallback")) {
        loaded.kind = SCHED_SOURCE_EMBEDDED_FALLBACK;
    } else if (!strcmp(scenario, "starting")) {
        snapshot_error = ESP_ERR_INVALID_STATE;
    } else if (!strcmp(scenario, "reload")) {
        reload = true;
    } else if (!strcmp(scenario, "reload_same_sha")) {
        reload_same_sha = true;
    } else if (!strcmp(scenario, "stale")) {
        release.release_metadata_verified = false;
    } else if (!strcmp(scenario, "wrong_file")) {
        release.sha256[0] = 'b';
    } else if (!strcmp(scenario, "missing_file")) {
        release_error = ESP_ERR_NOT_FOUND;
    } else if (!strcmp(scenario, "empty_release")) {
        release.version[0] = '\0';
    } else if (!strcmp(scenario, "no_workbook")) {
        loaded.has_workbook = false;
        strcpy(loaded.workbook, "-");
    } else if (!strcmp(scenario, "empty_workbook")) {
        loaded.workbook[0] = '\0';
    } else if (!strcmp(scenario, "dash_workbook")) {
        strcpy(loaded.workbook, "-");
    } else if (!strcmp(scenario, "missing_device")) {
        mac = "";
        cli_set_thing_name(NULL);
        description_missing = true;
    } else if (!strcmp(scenario, "malformed_sha")) {
        loaded.sha256[0] = 'G';
    } else if (!strcmp(scenario, "short_sha")) {
        loaded.sha256[3] = '\0';
    } else if (!strcmp(scenario, "unknown_source")) {
        loaded.kind = (sched_source_kind_t)99;
    } else if (!strcmp(scenario, "escaping")) {
        cli_set_thing_name("Field_\"\\\n\r\t\001é");
        strcpy(app.version, "fw_\"\\\n\r\t\037");
        strcpy(loaded.workbook, "wb_\"\\\n\r\t\002");
        strcpy(release.version, "rel_\"\\\n\r\t\003");
    } else if (!strcmp(scenario, "maximum_escaping")) {
        static char client[64];
        memset(client, 1, 63);
        cli_set_thing_name(client);
        memset(app.version, 2, 31);
        memset(loaded.workbook, 3, 63);
        memset(release.version, 4, 31);
        /* Longest location/timezone tail: 47-char names (the NVS cap),
         * every coordinate set and widest, the longest live status. */
        static const char longest[] = "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        tz_state.configured_set = tz_state.live_set = true;
        tz_state.configured_lat = tz_state.configured_lon = -179.999999;
        tz_state.live = (device_location_t){DEVICE_LOCATION_LIVE, -89.999999, -179.999999, 199999, 2591999, false};
        tz_state.location = tz_state.live;
        tz_state.location.configured_stale = true;
        strcpy(tz_state.configured, longest);
        strcpy(tz_state.resolved, longest);
        strcpy(tz_state.live_status, "not_authorized_xxxxxxxx");
        tz_state.source = "configured";
        active_zone = longest;
        active_offset = -43200;
    } else if (!strcmp(scenario, "prompt")) {
        fputs("ambyte> ", stdout);
    } else if (!strcmp(scenario, "log_tail")) {
        fputs("I (host) partial log tail", stdout);
    }

    pthread_t thread;
    bool race = !strcmp(scenario, "interleaving");
    if (race) {
        atomic_store(&race_enabled, true);
        assert(pthread_create(&thread, NULL, logger, NULL) == 0);
    }
    int repetitions = race ? 500 : 1;
    char *args[] = {"identity", "unexpected"};
    bool usage = !strcmp(scenario, "usage");
    for (int i = 0; i < repetitions; ++i) {
        int rc = cli_cmd_identity(usage ? 2 : 1, args);
        assert(rc == (usage ? 1 : 0));
    }
    if (race) {
        atomic_store(&done, true);
        assert(pthread_join(thread, NULL) == 0);
        assert(atomic_load(&attempted) && atomic_load(&blocked));
    }
    if (usage) assert(snapshot_calls == 0 && release_calls == 0);
    if (loaded.kind != SCHED_SOURCE_INSTALLED || snapshot_error != ESP_OK)
        assert(release_calls == 0);
    /* These dependencies do not exist in this executable: SD, network,
     * unlock, configuration getters/setters, queue operations and publisher.
     * Any new dependency is a link failure, rather than a permissive mock. */
    fprintf(stderr, "IDENTITY_HOST_OK snapshots=%u release_reads=%u\n",
            snapshot_calls, release_calls);
    return 0;
}
