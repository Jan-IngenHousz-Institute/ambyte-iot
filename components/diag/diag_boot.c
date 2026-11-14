/*
 * diag_boot.c — target-only glue: boot counter, reset reason, coredump
 * summary (read once, cached) and the esp_log hook installation.
 *
 * NVS namespace "diag" (separate from every other component's keys):
 *   boot_count  u32  incremented once per boot. One NVS write per boot is
 *                    the accepted cost; RTC_NOINIT would not survive the
 *                    power-on resets that matter most (brownouts).
 *   crash_id    u64  identity of the last coredump seen (see crash_identity)
 *   crash_boot  u32  boot_count on which crash_id was first seen
 */
#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "nvs.h"
#include "sdkconfig.h"

#define TAG "diag"
#define DIAG_NVS_NS        "diag"
#define DIAG_KEY_BOOTS     "boot_count"
#define DIAG_KEY_CRASH_ID  "crash_id"
#define DIAG_KEY_CRASH_BT  "crash_boot"

static const char *s_reset = "unknown";
static uint32_t    s_boot_count;
static char        s_crash_json[DIAG_CRASH_JSON_CAP] = "{\"present\":false,\"error\":\"not_checked\"}";

const char *diag_reset_reason(void) { return s_reset; }
uint32_t    diag_boot_count(void)   { return s_boot_count; }
const char *diag_crash_json(void)   { return s_crash_json; }

void diag_log_install(void)
{
    static bool installed;
    if (installed) return;
    installed = true;
    /* Chain order (diag_log.h): whatever is installed NOW — sd_logger's hook,
     * which tees to the console — becomes our downstream sink. */
    diag_log_set_next(esp_log_set_vprintf(diag_log_vprintf));
}

static uint32_t bump_boot_count(nvs_handle_t h, bool nvs_ok)
{
    if (!nvs_ok) return 0;
    uint32_t n = 0;
    esp_err_t err = nvs_get_u32(h, DIAG_KEY_BOOTS, &n);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        /* Never restart the count from 1 over an unreadable value: 0 says
         * "unknown" on the wire, a fresh 1 would look like a new unit. */
        ESP_LOGW(TAG, "boot counter unreadable: %s", esp_err_to_name(err));
        return 0;
    }
    n = (n == UINT32_MAX) ? UINT32_MAX : n + 1U;
    if (nvs_set_u32(h, DIAG_KEY_BOOTS, n) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "boot counter not persisted");
        return 0;
    }
    return n;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
/* Stable identity of the dump image without hashing 64 KiB: its size and a
 * CRC of its last ≤32 bytes, which hold the image checksum IDF wrote over the
 * whole dump (CRC32 here; SHA-256 if that Kconfig is ever chosen). Two
 * different dumps collide only if both size and checksum tail do. Works for a
 * corrupt image too, so a corrupt dump still has a first_seen_boot. */
static uint64_t crash_identity(const esp_partition_t *part, uint32_t size)
{
    uint8_t tail[32];
    const uint32_t n = size < sizeof tail ? size : (uint32_t)sizeof tail;
    if (n == 0 || esp_partition_read(part, size - n, tail, n) != ESP_OK) return 0;
    const uint64_t id = ((uint64_t)size << 32) | esp_rom_crc32_le(0, tail, n);
    return id != 0 ? id : 1;
}

static void read_crash(diag_crash_t *c, nvs_handle_t h, bool nvs_ok, uint32_t boot)
{
    memset(c, 0, sizeof *c);
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (part == NULL) {
        c->error = "no_partition";
        return;
    }
    /* The first word is the image size; an erased partition reads all-ones.
     * Read it ourselves: esp_core_dump_image_get() returns the same
     * ESP_ERR_INVALID_SIZE for "blank" and for "corrupt size". */
    uint32_t size = 0;
    esp_err_t err = esp_partition_read(part, 0, &size, sizeof size);
    if (err != ESP_OK) {
        c->present = true;               /* cannot prove absence */
        c->error = esp_err_to_name(err);
        return;
    }
    if (size == 0xFFFFFFFFu) return;     /* blank: no dump */
    c->present = true;
    if (size < sizeof(uint32_t) || size > part->size) {
        c->error = "invalid_size";
        return;
    }
    c->id = crash_identity(part, size);
    if (c->id != 0) {
        uint64_t seen_id = 0;
        uint32_t seen_boot = 0;
        if (nvs_ok && nvs_get_u64(h, DIAG_KEY_CRASH_ID, &seen_id) == ESP_OK && seen_id == c->id &&
            nvs_get_u32(h, DIAG_KEY_CRASH_BT, &seen_boot) == ESP_OK) {
            c->first_seen_valid = true;
            c->first_seen_boot = seen_boot;
        } else if (nvs_ok && boot != 0 &&
                   nvs_set_u64(h, DIAG_KEY_CRASH_ID, c->id) == ESP_OK &&
                   nvs_set_u32(h, DIAG_KEY_CRASH_BT, boot) == ESP_OK &&
                   nvs_commit(h) == ESP_OK) {
            c->first_seen_valid = true;
            c->first_seen_boot = boot;
        }
    }
    /* Verify BEFORE parsing: esp_core_dump_get_summary() walks ELF headers
     * straight out of the mmapped partition with no bounds checks, so a
     * corrupt image could fault the parser. Only a CRC-clean image is parsed. */
    err = esp_core_dump_image_check();
    if (err != ESP_OK) {
        c->error = esp_err_to_name(err);
        return;
    }
    esp_core_dump_summary_t *s = calloc(1, sizeof *s);   /* ~330 B; task context */
    if (s == NULL) {
        c->error = "no_mem";
        return;
    }
    err = esp_core_dump_get_summary(s);
    if (err != ESP_OK) {
        c->error = esp_err_to_name(err);
        free(s);
        return;
    }
    c->summary_valid = true;
    memcpy(c->app_elf_sha256, s->app_elf_sha256, DIAG_CRASH_SHA_HEX);
    c->app_elf_sha256[DIAG_CRASH_SHA_HEX] = '\0';
    strncpy(c->task, s->exc_task, sizeof c->task - 1);
    c->pc = s->exc_pc;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    c->exc_cause = s->ex_info.exc_cause;
    c->exc_vaddr = s->ex_info.exc_vaddr;
    c->bt_depth = s->exc_bt_info.depth;
    c->bt_corrupted = s->exc_bt_info.corrupted;
    for (uint32_t i = 0; i < DIAG_CRASH_BT_MAX && i < s->exc_bt_info.depth; i++) {
        c->backtrace[i] = s->exc_bt_info.bt[i];
    }
#endif
    free(s);
}
#else
static void read_crash(diag_crash_t *c, nvs_handle_t h, bool nvs_ok, uint32_t boot)
{
    (void)h; (void)nvs_ok; (void)boot;
    memset(c, 0, sizeof *c);
    c->error = "coredump_disabled";
}
#endif

void diag_boot_init(void)
{
    static bool done;
    if (done) return;
    done = true;

    s_reset = diag_reset_reason_name(esp_reset_reason());
    nvs_handle_t h = 0;
    const bool nvs_ok = nvs_open(DIAG_NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    s_boot_count = bump_boot_count(h, nvs_ok);

    diag_crash_t c;
    read_crash(&c, h, nvs_ok, s_boot_count);
    if (nvs_ok) nvs_close(h);
    if (diag_crash_render_json(&c, s_crash_json, sizeof s_crash_json) < 0) {
        /* Cannot happen within DIAG_CRASH_JSON_CAP; keep a valid object anyway. */
        snprintf(s_crash_json, sizeof s_crash_json, "{\"present\":%s,\"error\":\"render_failed\"}",
                 c.present ? "true" : "false");
    }
    ESP_LOGW(TAG, "boot #%lu reset_reason=%s crash=%s", (unsigned long)s_boot_count, s_reset, s_crash_json);
}
