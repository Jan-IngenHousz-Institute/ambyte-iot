#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "CLI.h"
#include "cli_identity.h"
#include "device_commands.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sched_runner.h"
#include "script_update.h"
#include "ota_identity.h"
#include "timezone.h"
#include "timezone_command.h"
#include <math.h>
#include <time.h>

static const char *s_thing_name;

void cli_set_thing_name(const char *thing_name)
{
    s_thing_name = thing_name;
}

typedef struct {
    /* At most 6 bytes per escaped character: MAC 17, client id 63, compiled
     * fw 31, workbook 63, release version 31, plus fixed keys/tokens and SHA,
     * plus the location/timezone tail (three <=47-char zone names; the
     * worst case is pinned by the maximum_escaping host scenario).
     * Keep the whole record off the heap and below the console's 12 KiB stack. */
    char data[3072];
    size_t len;
    bool overflow;
} identity_json_t;

static void json_raw(identity_json_t *w, const char *value)
{
    size_t n = strlen(value);
    if (w->overflow || n >= sizeof w->data - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->data + w->len, value, n);
    w->len += n;
    w->data[w->len] = '\0';
}

static bool valid_utf8(const unsigned char *p)
{
    while (*p) {
        unsigned char lead = *p++;
        unsigned continuation;
        if (lead < 0x80) continue;
        if (lead >= 0xc2 && lead <= 0xdf) continuation = 1;
        else if (lead >= 0xe0 && lead <= 0xef) continuation = 2;
        else if (lead >= 0xf0 && lead <= 0xf4) continuation = 3;
        else return false;

        /* Reject overlong encodings, surrogate code points and values above
         * U+10FFFF. Check each continuation before advancing, so a truncated
         * sequence never reads beyond the string's terminating NUL. */
        if ((lead == 0xe0 && *p < 0xa0) ||
            (lead == 0xed && *p > 0x9f) ||
            (lead == 0xf0 && *p < 0x90) ||
            (lead == 0xf4 && *p > 0x8f)) return false;
        for (unsigned i = 0; i < continuation; ++i) {
            if (*p < 0x80 || *p > 0xbf) return false;
            ++p;
        }
    }
    return true;
}

static void json_string(identity_json_t *w, const char *value)
{
    /* Existing cfg/commit setters accept bytes without checking encoding.
     * An unrepresentable identifier is unavailable, rather than a repaired
     * identifier that could incorrectly match a different device/workbook. */
    if (value == NULL || value[0] == '\0' ||
        !valid_utf8((const unsigned char *)value)) {
        json_raw(w, "null");
        return;
    }
    json_raw(w, "\"");
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        char escaped[7] = {0};
        if (*p == '"' || *p == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*p;
        } else if (*p < 0x20) {
            static const char hex[] = "0123456789abcdef";
            memcpy(escaped, "\\u00", 4);
            escaped[4] = hex[*p >> 4];
            escaped[5] = hex[*p & 15];
        } else {
            escaped[0] = (char)*p;
        }
        json_raw(w, escaped);
    }
    json_raw(w, "\"");
}

static bool valid_sha(const char *sha)
{
    if (strlen(sha) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        if (!((sha[i] >= '0' && sha[i] <= '9') ||
              (sha[i] >= 'a' && sha[i] <= 'f'))) return false;
    }
    return true;
}

static bool same_program(const sched_identity_t *a, const sched_identity_t *b)
{
    return a->generation == b->generation && a->kind == b->kind &&
           a->has_workbook == b->has_workbook &&
           strcmp(a->sha256, b->sha256) == 0 &&
           strcmp(a->workbook, b->workbook) == 0;
}

static void json_schedule(identity_json_t *w)
{
    static const char *const sources[] = {
        "NONE", "INSTALLED", "EMBEDDED_DEFAULT", "EMBEDDED_FALLBACK",
    };
    sched_identity_t loaded = {0}, after = {0};
    script_identity_t release = {0};
    bool available = sched_runner_identity(&loaded) == ESP_OK &&
                     (unsigned)loaded.kind < sizeof sources / sizeof sources[0];
    bool verified = false;
    if (available && loaded.kind == SCHED_SOURCE_INSTALLED) {
        verified = script_update_get_identity(&release) == ESP_OK &&
                   release.release_metadata_verified && valid_sha(loaded.sha256) &&
                   strcmp(loaded.sha256, release.sha256) == 0;
        /* Hashing the installed file/NVS is outside the runner lock. A new
         * generation in that interval invalidates the entire tuple, even if
         * its bytes happen to have the same SHA as the preceding program. */
        available = sched_runner_identity(&after) == ESP_OK &&
                    same_program(&loaded, &after);
    }

    json_raw(w, "{\"source\":");
    json_string(w, available ? sources[loaded.kind] : NULL);
    json_raw(w, ",\"sha256\":");
    json_string(w, available && loaded.kind != SCHED_SOURCE_NONE &&
                   valid_sha(loaded.sha256) ? loaded.sha256 : NULL);
    json_raw(w, ",\"workbookVersionId\":");
    json_string(w, available && loaded.kind != SCHED_SOURCE_NONE &&
                   loaded.has_workbook && strcmp(loaded.workbook, "-") != 0
                   ? loaded.workbook : NULL);
    json_raw(w, ",\"script_version\":");
    json_string(w, available && verified ? release.version : NULL);
    json_raw(w, "}");
}

/* Location + timezone provenance (additive since the location resolver;
 * docs/timezone-from-location.md). `location` is the EFFECTIVE location with
 * its source; `configured` is the persisted fallback and `live` the online
 * estimate, both always shown so a tool can tell real deployment
 * coordinates from defaults. time_sync's compiled 52.173N 5.819E sun-math
 * default is never reported as a location. */
static void json_number_or_null(identity_json_t *w, const char *key, bool set, double v, const char *fmt)
{
    char buf[48];
    json_raw(w, key);
    if (set && isfinite(v)) {
        snprintf(buf, sizeof buf, fmt, v);
        json_raw(w, buf);
    } else {
        json_raw(w, "null");
    }
}

static void json_location_and_timezone(identity_json_t *w)
{
    timezone_status_t st;
    timezone_command_status(&st);
    bool set = st.location.source != DEVICE_LOCATION_NONE;
    char buf[64];

    json_raw(w, ",\"location\":{\"set\":");
    json_raw(w, set ? "true" : "false");
    json_raw(w, ",\"source\":");
    json_string(w, device_location_source_name(st.location.source));
    json_number_or_null(w, ",\"lat\":", set, st.location.lat, "%.6f");
    json_number_or_null(w, ",\"lon\":", set, st.location.lon, "%.6f");
    json_number_or_null(w, ",\"accuracy_m\":", set, st.location.accuracy_m, "%.0f");
    json_raw(w, ",\"configured_stale\":");
    json_raw(w, st.location.configured_stale ? "true" : "false");
    json_raw(w, ",\"configured\":{\"set\":");
    json_raw(w, st.configured_set ? "true" : "false");
    json_number_or_null(w, ",\"lat\":", st.configured_set, st.configured_lat, "%.6f");
    json_number_or_null(w, ",\"lon\":", st.configured_set, st.configured_lon, "%.6f");
    json_raw(w, "},\"live\":{\"set\":");
    json_raw(w, st.live_set ? "true" : "false");
    json_number_or_null(w, ",\"lat\":", st.live_set, st.live.lat, "%.6f");
    json_number_or_null(w, ",\"lon\":", st.live_set, st.live.lon, "%.6f");
    json_number_or_null(w, ",\"accuracy_m\":", st.live_set, st.live.accuracy_m, "%.0f");
    json_number_or_null(w, ",\"age_s\":", st.live_set && st.live.age_s >= 0, (double)st.live.age_s, "%.0f");
    json_raw(w, ",\"status\":");
    json_string(w, st.live_status);
    json_raw(w, "}}");

    const char *active;
    int32_t offset;
    timezone_snapshot((int64_t)time(NULL), &active, &offset);
    json_raw(w, ",\"timezone\":{\"configured\":");
    json_string(w, st.configured);
    json_raw(w, ",\"active\":");
    json_string(w, active);
    json_raw(w, ",\"source\":");
    json_string(w, st.source);
    json_raw(w, ",\"pinned\":");
    json_raw(w, st.pinned ? "true" : "false");
    json_raw(w, ",\"resolved\":");
    json_string(w, st.resolved);
    snprintf(buf, sizeof buf, ",\"utc_offset_seconds\":%ld}", (long)offset);
    json_raw(w, buf);
}

int cli_cmd_identity(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: identity\r\n");
        return 1;
    }
    identity_json_t w = {0};
    const esp_app_desc_t *app = esp_app_get_description();
    json_raw(&w, "{\"mac\":");
    json_string(&w, device_commands_get_mac());
    json_raw(&w, ",\"thing_name\":");
    json_string(&w, s_thing_name);
    json_raw(&w, ",\"fw\":");
    json_string(&w, app != NULL ? app->version : NULL);
    json_raw(&w, ",\"contract\":1,\"schedule\":");
    json_schedule(&w);
    json_location_and_timezone(&w);
    json_raw(&w, ",\"capabilities\":[\"identity\",\"cfg.get\",\"cfg.set\",\"reboot\","
                 "\"schedule.status\",\"schedule.release\",\"schedule.begin\","
                 "\"schedule.put\",\"schedule.commit\",\"schedule.abort\",\"svc\",\"sensors\","
                 "\"timezone.auto\"]");
    char firmware_identity[192];
    if (!ota_running_identity_json(firmware_identity, sizeof firmware_identity)) return 1;
    json_raw(&w, ",");
    json_raw(&w, firmware_identity);
    json_raw(&w, "}");
    if (w.overflow) {
        printf("identity: response exceeds buffer\r\n");
        return 1;
    }

    /* esp_log v1's sd_logger tee ultimately uses vprintf on this same FILE.
     * Hold its lock only for output, including a separator for an unfinished
     * prompt/log tail. ROM/early/panic writers bypass stdio; clients still
     * discard any physical line that does not parse as a complete object. */
    flockfile(stdout);
    bool ok = fputs("\r\n", stdout) >= 0;
    if (fwrite(w.data, 1, w.len, stdout) != w.len) ok = false;
    if (fputs("\r\n", stdout) < 0 || fflush(stdout) != 0) ok = false;
    funlockfile(stdout);

    ESP_LOGI("identity", "budget internal_free=%u internal_largest=%u "
             "internal_min=%u cli_stack_min=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return ok ? 0 : 1;
}
