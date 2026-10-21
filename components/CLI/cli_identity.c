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

static const char *s_thing_name;

void cli_set_thing_name(const char *thing_name)
{
    s_thing_name = thing_name;
}

typedef struct {
    /* At most 6 bytes per escaped character: MAC 17, client id 63, compiled
     * fw 31, workbook 63, release version 31, plus fixed keys/tokens and SHA.
     * Keep the whole record off the heap and below the console's 12 KiB stack. */
    char data[2048];
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
    json_raw(&w, ",\"capabilities\":[\"identity\",\"cfg.get\",\"cfg.set\",\"reboot\","
                 "\"schedule.status\",\"schedule.release\",\"schedule.begin\","
                 "\"schedule.put\",\"schedule.commit\",\"schedule.abort\"]}");
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
