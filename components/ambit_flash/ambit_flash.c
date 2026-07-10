/*
 * ambit_flash.c — host-driven ROM flasher for the AMBIT (ESP8685 / ESP32-C3).
 *
 * Phase-2 slice: ambit_flash_probe() proves the end-to-end path — enter ROM
 * download mode over the FFC, connect via esp-serial-flasher, read chip + MAC,
 * reset back to the app. Full multi-region flashing builds on the same session.
 */

#include "ambit_flash.h"
#include "ambit_flash_port.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "esp_loader.h"
#include "uart_sensors.h"
#include "sd_card.h"
#include "device_commands.h"   /* cmd_uart_ping, cmd_ambit_get_info */
#include "ambit_protocol.h"    /* AMBIT_INFO_FW, ambit_fw_info_t */

#define AMBIT_FW_ROOT  "/sdcard/ambit_fw"

#define TAG            "ambit_flash"
#define FLASH_UART     UART_NUM_0
#define ROM_BAUD       115200      /* ESP32-C3 ROM starts here; matches uart_sensors */
#define BUS_WAIT_MS    3000
#define FLASH_BLOCK    1024        /* per esp_loader_flash_write block (matches ref example) */
#define FLASH_BAUD_DEF 460800      /* post-stub link speed (short FFC); override via `baud` */

/* AMBIT (ESP8685/C3) flash layout — same regions/offsets the factory jig writes
 * (ambit-IoT/src/uploader.py). NVS at 0x9000 is deliberately NOT in this list, so
 * a full flash preserves per-unit calibration. */
typedef struct {
    uint32_t    offset;
    const char *fname;
} flash_region_t;

static const flash_region_t s_regions[] = {
    { 0x0,     "bootloader.bin" },
    { 0x8000,  "partitions.bin" },
    { 0xe000,  "boot_app0.bin"  },
    { 0x10000, "app.bin"        },
};
#define NUM_REGIONS (sizeof(s_regions) / sizeof(s_regions[0]))

/* Serialized by the flash session — safe as a single static scratch buffer. */
static uint8_t s_flash_buf[FLASH_BLOCK];

esp_err_t ambit_flash_probe(uint8_t channel, ambit_flash_probe_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }

    /* Take the shared bus + route UART0 to this channel for the whole session. */
    esp_err_t e = uart_sensors_flash_session_begin(channel, BUS_WAIT_MS);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ch%u: cannot take UART bus (%s) — is Lua still running?",
                 channel, esp_err_to_name(e));
        return e;
    }

    ambit_flash_port_t port = {
        .base    = { .ops = &ambit_flash_port_ops },
        .channel = channel,
    };
    esp_loader_t loader;
    esp_err_t ret = ESP_FAIL;

    esp_loader_error_t le = esp_loader_init_serial(&loader, &port.base);
    if (le != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "ch%u: esp_loader_init_serial failed (%d)", channel, le);
    } else {
        /* connect() calls our enter_bootloader (reset/boot sequence) then SYNC. */
        esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
        le = esp_loader_connect(&loader, &args);
        if (le == ESP_LOADER_SUCCESS) {
            target_chip_t chip = esp_loader_get_target(&loader);
            uint8_t mac[6] = {0};
            esp_loader_read_mac(&loader, mac);   /* best-effort */
            ESP_LOGW(TAG, "ch%u: AMBIT ROM connected — chip=%d "
                          "MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                     channel, (int)chip,
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            if (out) {
                out->connected = true;
                out->chip      = (int)chip;
                memcpy(out->mac, mac, sizeof(out->mac));
            }
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "ch%u: ROM connect failed (loader err %d) — no SYNC response",
                     channel, le);
        }
        /* Always return the AMBIT to its application. */
        esp_loader_reset_target(&loader);
    }

    /* Restore the link baud in case a higher rate was negotiated, then free the bus. */
    uart_set_baudrate(FLASH_UART, ROM_BAUD);
    uart_sensors_flash_session_end(channel);
    return ret;
}

/* ── full multi-region ROM flash ─────────────────────────────────────────── */

/* Open dir/fname, return its size (>0) or 0 on error. */
static long region_file_size(const char *dir, const char *fname)
{
    char path[192];
    snprintf(path, sizeof path, "%s/%s", dir, fname);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return (sz > 0) ? sz : 0;
}

/* Stream one region file to flash: start (erase) -> write blocks -> finish (MD5). */
static esp_loader_error_t flash_one_region(esp_loader_t *loader, const char *dir,
                                           const flash_region_t *r, uint32_t *bytes_out)
{
    char path[192];
    snprintf(path, sizeof path, "%s/%s", dir, r->fname);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_LOADER_ERROR_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(f);
        ESP_LOGE(TAG, "%s is empty", path);
        return ESP_LOADER_ERROR_FAIL;
    }

    ESP_LOGW(TAG, "  region %s @0x%06lx: %ld B (erasing…)", r->fname,
             (unsigned long)r->offset, fsize);

    esp_loader_flash_cfg_t cfg = {
        .offset     = r->offset,
        .image_size = (uint32_t)fsize,
        .block_size = FLASH_BLOCK,
    };
    esp_loader_error_t le = esp_loader_flash_start(loader, &cfg);
    if (le != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "  flash_start(%s) failed (%d)", r->fname, le);
        fclose(f);
        return le;
    }

    uint32_t written = 0;
    int last_decile = -1;
    size_t n;
    while ((n = fread(s_flash_buf, 1, sizeof s_flash_buf, f)) > 0) {
        le = esp_loader_flash_write(loader, &cfg, s_flash_buf, (uint32_t)n);
        if (le != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "  flash_write(%s) failed at %lu B (%d)",
                     r->fname, (unsigned long)written, le);
            fclose(f);
            return le;
        }
        written += (uint32_t)n;
        int decile = (int)((uint64_t)written * 10 / (uint32_t)fsize);
        if (decile != last_decile) {
            ESP_LOGW(TAG, "    %s %d%%", r->fname, decile * 10);
            last_decile = decile;
        }
    }
    fclose(f);

    le = esp_loader_flash_finish(loader, &cfg);   /* MD5 verify (skip_verify=false) */
    if (le != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "  flash_finish/verify(%s) failed (%d)", r->fname, le);
        return le;
    }
    ESP_LOGW(TAG, "  region %s verified", r->fname);
    if (bytes_out) {
        *bytes_out = written;
    }
    return ESP_LOADER_SUCCESS;
}

esp_err_t ambit_flash_image(uint8_t channel, const char *dir, uint32_t baud,
                            ambit_flash_image_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (dir == NULL || channel >= UART_SENSOR_NUM_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (baud == 0) {
        baud = FLASH_BAUD_DEF;
    }

    /* The SD card must be present + mounted to read the region files. After a
     * hot-swap the mount may be down — try once to (re)mount. */
    if (!sdcard_is_mounted() && sdcard_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD card not available — insert it into the ambyte "
                      "(reboot if it was hot-swapped)");
        return ESP_ERR_INVALID_STATE;
    }

    /* Fail fast: all four region files must be present + non-empty BEFORE we touch
     * the chip, so we never half-flash because one file was missing. */
    for (size_t i = 0; i < NUM_REGIONS; i++) {
        if (region_file_size(dir, s_regions[i].fname) == 0) {
            ESP_LOGE(TAG, "missing/empty %s/%s — need all 4 region files",
                     dir, s_regions[i].fname);
            return ESP_ERR_NOT_FOUND;
        }
    }

    esp_err_t e = uart_sensors_flash_session_begin(channel, BUS_WAIT_MS);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ch%u: cannot take UART bus (%s) — is Lua still running?",
                 channel, esp_err_to_name(e));
        return e;
    }

    ambit_flash_port_t port = {
        .base    = { .ops = &ambit_flash_port_ops },
        .channel = channel,
    };
    esp_loader_t loader;
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    target_chip_t chip = ESP_UNKNOWN_CHIP;
    esp_err_t ret = ESP_FAIL;
    int regions_done = 0;
    uint32_t total = 0;

    esp_loader_error_t le = esp_loader_init_serial(&loader, &port.base);
    if (le != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "ch%u: init_serial failed (%d)", channel, le);
        goto done;
    }

    /* Stub upload enables faster, block-verified writes; then raise the link baud. */
    le = esp_loader_connect_with_stub(&loader, &args);
    if (le != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "ch%u: ROM stub connect failed (%d)", channel, le);
        goto reset_and_done;
    }

    chip = esp_loader_get_target(&loader);
    if (out) {
        out->chip = (int)chip;
    }
    if (chip != ESP32C3_CHIP) {
        ESP_LOGE(TAG, "ch%u: unexpected target chip=%d (expected C3/ESP8685=3) — aborting",
                 channel, (int)chip);
        le = ESP_LOADER_ERROR_INVALID_TARGET;
        goto reset_and_done;
    }

    le = esp_loader_change_transmission_rate(&loader, baud);
    if (le != ESP_LOADER_SUCCESS) {
        ESP_LOGW(TAG, "ch%u: baud change to %lu failed (%d) — continuing at %d",
                 channel, (unsigned long)baud, le, ROM_BAUD);
        /* Non-fatal: the port stayed at the ROM baud, keep flashing slower. */
        le = ESP_LOADER_SUCCESS;
    } else {
        ESP_LOGW(TAG, "ch%u: link raised to %lu baud", channel, (unsigned long)baud);
    }

    ESP_LOGW(TAG, "ch%u: flashing %u regions from %s", channel, (unsigned)NUM_REGIONS, dir);
    for (size_t i = 0; i < NUM_REGIONS; i++) {
        uint32_t nb = 0;
        le = flash_one_region(&loader, dir, &s_regions[i], &nb);
        if (le != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "ch%u: region %s FAILED — aborting (chip may be partially written)",
                     channel, s_regions[i].fname);
            goto reset_and_done;
        }
        regions_done++;
        total += nb;
    }
    ret = ESP_OK;
    ESP_LOGW(TAG, "ch%u: FLASH COMPLETE — %d regions, %lu bytes", channel,
             regions_done, (unsigned long)total);

reset_and_done:
    /* Always return the AMBIT to running its (new or old) application. */
    esp_loader_reset_target(&loader);
done:
    uart_set_baudrate(FLASH_UART, ROM_BAUD);
    uart_sensors_flash_session_end(channel);
    if (out) {
        out->ok              = (ret == ESP_OK);
        out->regions_written = regions_done;
        out->total_bytes     = total;
    }
    return ret;
}

/* ── version check (detect + report; no flashing) ────────────────────────── */

static bool ver_gt(uint8_t a, uint8_t b, uint8_t c, uint8_t x, uint8_t y, uint8_t z)
{
    if (a != x) return a > x;
    if (b != y) return b > y;
    return c > z;
}

static bool fw_matches(const ambit_fw_info_t *fw, const ambit_flash_target_t *tgt)
{
    return fw->major == tgt->major && fw->minor == tgt->minor && fw->batch == tgt->batch;
}

static bool dir_has_all_regions(const char *dir)
{
    for (size_t i = 0; i < NUM_REGIONS; i++) {
        if (region_file_size(dir, s_regions[i].fname) == 0) {
            return false;
        }
    }
    return true;
}

esp_err_t ambit_flash_find_target(ambit_flash_target_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sdcard_is_mounted() && sdcard_mount() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    DIR *d = opendir(AMBIT_FW_ROOT);
    if (d == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    bool found = false;
    ambit_flash_target_t best = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        unsigned mj, mn, bt;
        char     tail;
        /* Accept exactly "N.N.N" — the trailing %c catches "0.0.7.bak" etc. */
        if (sscanf(e->d_name, "%u.%u.%u%c", &mj, &mn, &bt, &tail) != 3) continue;
        if (mj > 255 || mn > 255 || bt > 255) continue;
        /* Rebuild the path from the parsed numbers (bounded; also enforces canonical
         * "major.minor.batch" folder names — non-canonical spellings won't match). */
        char dir[96];
        snprintf(dir, sizeof dir, "%s/%u.%u.%u", AMBIT_FW_ROOT, mj, mn, bt);
        if (!dir_has_all_regions(dir)) continue;
        if (!found || ver_gt((uint8_t)mj, (uint8_t)mn, (uint8_t)bt,
                             best.major, best.minor, best.batch)) {
            best.major = (uint8_t)mj;
            best.minor = (uint8_t)mn;
            best.batch = (uint8_t)bt;
            snprintf(best.dir, sizeof best.dir, "%s", dir);
            found = true;
        }
    }
    closedir(d);

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = best;
    return ESP_OK;
}

int ambit_flash_check(void)
{
    ambit_flash_target_t tgt;
    esp_err_t e = ambit_flash_find_target(&tgt);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no SD target image under %s — put a build in a "
                      "<major.minor.batch> folder with the 4 region files", AMBIT_FW_ROOT);
        return -1;
    }
    ESP_LOGW(TAG, "SD target: v%u.%u.%u (%s)", tgt.major, tgt.minor, tgt.batch, tgt.dir);

    int mismatches = 0;
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        bool connected = false;
        cmd_result_t pr = cmd_uart_ping(ch, &connected);
        if (pr.status != ESP_OK || !connected) {
            ESP_LOGI(TAG, "  ch%u: absent", ch);
            continue;
        }
        uint8_t buf[64];
        size_t  len = 0;
        cmd_result_t r = cmd_ambit_get_info(ch, AMBIT_INFO_FW, buf, sizeof buf, &len);
        if (r.status != ESP_OK || len < sizeof(ambit_fw_info_t)) {
            ESP_LOGW(TAG, "  ch%u: present but version unreadable (%s)",
                     ch, esp_err_to_name(r.status));
            continue;
        }
        const ambit_fw_info_t *fw = (const ambit_fw_info_t *)buf;
        if (fw_matches(fw, &tgt)) {
            ESP_LOGI(TAG, "  ch%u: v%u.%u.%u — up to date", ch, fw->major, fw->minor, fw->batch);
        } else {
            mismatches++;
            ESP_LOGW(TAG, "  ch%u: v%u.%u.%u != target v%u.%u.%u — run: ambit_flash %u %u.%u.%u",
                     ch, fw->major, fw->minor, fw->batch,
                     tgt.major, tgt.minor, tgt.batch,
                     ch, tgt.major, tgt.minor, tgt.batch);
        }
    }
    ESP_LOGW(TAG, "AMBIT version check: %d channel(s) differ from the SD target", mismatches);
    return mismatches;
}

/* ── boot-time auto-sync (Phase 3 full) ──────────────────────────────────── */

/* Anti-brick-loop latch, NVS ns "ambit_fl", key "bf<ch>" = "<M.m.b>:<fails>".
 * Counts attempts that did not end with the channel VERIFIED at the target —
 * covering both flash failures and the mislabelled-folder case (flash verifies
 * but the app inside reports a different version than the folder name, which
 * would otherwise reflash on every boot). Staging a different target version
 * resets the count; a verified flash clears it. */
#define BOOT_NVS_NS          "ambit_fl"
#define BOOT_FAIL_MAX        3
/* device_commands' publish power gate opens after 15 s of continuous external
 * power (5 s eval cache) — poll a little past that before giving up. */
#define BOOT_GATE_WAIT_MS    30000
#define BOOT_GATE_POLL_MS    2000
/* AMBIT app boot time after the post-flash reset, before the version verify. */
#define BOOT_VERIFY_DELAY_MS 4000
#define BOOT_VERIFY_TRIES    2

static void boot_fail_key(uint8_t ch, char *key, size_t cap)
{
    snprintf(key, cap, "bf%u", ch);
}

static int boot_fail_count(uint8_t ch, const char *ver)
{
    nvs_handle_t h;
    if (nvs_open(BOOT_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    char key[8], val[24];
    boot_fail_key(ch, key, sizeof key);
    size_t len = sizeof val;
    esp_err_t e = nvs_get_str(h, key, val, &len);
    nvs_close(h);
    if (e != ESP_OK) {
        return 0;
    }
    char stored_ver[16];
    int  fails = 0;
    if (sscanf(val, "%15[^:]:%d", stored_ver, &fails) != 2 ||
        strcmp(stored_ver, ver) != 0) {
        return 0;   /* different target than the one that failed — start over */
    }
    return fails;
}

static void boot_fail_bump(uint8_t ch, const char *ver, int prev)
{
    nvs_handle_t h;
    if (nvs_open(BOOT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[8], val[24];
    boot_fail_key(ch, key, sizeof key);
    snprintf(val, sizeof val, "%s:%d", ver, prev + 1);
    if (nvs_set_str(h, key, val) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static void boot_fail_clear(uint8_t ch)
{
    nvs_handle_t h;
    if (nvs_open(BOOT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[8];
    boot_fail_key(ch, key, sizeof key);
    if (nvs_erase_key(h, key) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* Read one channel's running fw version via cmd 33/2. do_query does its own
 * wake, so this works regardless of a cached ping failure (a bare unit's boot
 * ping is fail-cached for 5 min — the post-flash verify must not consult it). */
static bool boot_read_version(uint8_t ch, ambit_fw_info_t *fw)
{
    uint8_t buf[64];
    size_t  len = 0;
    cmd_result_t r = cmd_ambit_get_info(ch, AMBIT_INFO_FW, buf, sizeof buf, &len);
    if (r.status != ESP_OK || len < sizeof(ambit_fw_info_t)) {
        return false;
    }
    memcpy(fw, buf, sizeof *fw);
    return true;
}

int ambit_flash_boot_sync(void)
{
    ambit_flash_target_t tgt;
    if (ambit_flash_find_target(&tgt) != ESP_OK) {
        ESP_LOGI(TAG, "boot sync: no SD target image under %s — skipping", AMBIT_FW_ROOT);
        return -1;
    }
    char ver[16];
    snprintf(ver, sizeof ver, "%u.%u.%u", tgt.major, tgt.minor, tgt.batch);
    ESP_LOGW(TAG, "boot sync: SD target v%s (%s)", ver, tgt.dir);

    /* Pass 1: app-level version read for every channel (harmless to running
     * units). Silent channels are remembered for the ROM-probe pass. */
    enum { CH_OK, CH_STALE, CH_SILENT, CH_BARE, CH_ABSENT, CH_SKIPPED };
    int state[UART_SENSOR_NUM_CHANNELS];
    int fails[UART_SENSOR_NUM_CHANNELS];   /* -1 = not read from NVS yet */
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        fails[ch] = -1;
        bool connected = false;
        (void)cmd_uart_ping(ch, &connected);
        if (!connected) {
            state[ch] = CH_SILENT;
            continue;
        }
        ambit_fw_info_t fw;
        if (!boot_read_version(ch, &fw)) {
            state[ch] = CH_SKIPPED;
            ESP_LOGW(TAG, "  ch%u: present but version unreadable — not auto-flashing; "
                          "run: ambit_flash %u %s", ch, ch, ver);
            continue;
        }
        if (fw_matches(&fw, &tgt)) {
            state[ch] = CH_OK;
            ESP_LOGI(TAG, "  ch%u: v%u.%u.%u — up to date", ch, fw.major, fw.minor, fw.batch);
        } else {
            state[ch] = CH_STALE;
            ESP_LOGW(TAG, "  ch%u: v%u.%u.%u != target v%s — will auto-flash",
                     ch, fw.major, fw.minor, fw.batch, ver);
        }
    }

    /* Pass 2: ROM-probe the silent channels — a bare/bricked unit has no app to
     * answer the ping but its ROM bootloader always answers SYNC. NOTE: each
     * probe hard-resets ALL FOUR AMBITs (shared enable line); that is why this
     * whole function must run before Lua starts measuring — and why fail-capped
     * channels are skipped BEFORE the probe (a capped channel would otherwise
     * cost seconds + a gratuitous reset of the whole bank on every boot). */
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        if (state[ch] != CH_SILENT) {
            continue;
        }
        fails[ch] = boot_fail_count(ch, ver);
        if (fails[ch] >= BOOT_FAIL_MAX) {
            state[ch] = CH_SKIPPED;
            ESP_LOGE(TAG, "  ch%u: silent, %d failed auto-flash attempts for v%s — "
                          "giving up on this target; run: ambit_flash %u %s",
                     ch, fails[ch], ver, ch, ver);
            continue;
        }
        ambit_flash_probe_result_t pr;
        if (ambit_flash_probe(ch, &pr) != ESP_OK) {
            state[ch] = CH_ABSENT;
            ESP_LOGI(TAG, "  ch%u: absent", ch);
            continue;
        }
        /* ROM answered, so a unit is physically there. Before declaring it bare,
         * re-try the app version read (the probe reset it into its app; give it
         * time to boot): the boot auto-ping's failure is fail-cached for 5 min,
         * so a unit that was merely still booting then would otherwise be
         * misclassified as bare and needlessly reflashed. boot_read_version
         * bypasses the ping cache (do_query does its own wake). */
        vTaskDelay(pdMS_TO_TICKS(BOOT_VERIFY_DELAY_MS));
        ambit_fw_info_t fw;
        if (boot_read_version(ch, &fw)) {
            if (fw_matches(&fw, &tgt)) {
                state[ch] = CH_OK;
                ESP_LOGI(TAG, "  ch%u: v%u.%u.%u — up to date (ping glitch, app alive)",
                         ch, fw.major, fw.minor, fw.batch);
            } else {
                state[ch] = CH_STALE;
                ESP_LOGW(TAG, "  ch%u: v%u.%u.%u != target v%s — will auto-flash",
                         ch, fw.major, fw.minor, fw.batch, ver);
            }
        } else {
            state[ch] = CH_BARE;
            ESP_LOGW(TAG, "  ch%u: no app response but ROM answered — bare/bricked "
                          "unit, will auto-flash", ch);
        }
    }

    /* Fail-cap filter (pass-1 stale channels) + tally. */
    int n_need = 0, n_skipped = 0;
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        if (state[ch] == CH_SKIPPED) {
            n_skipped++;
            continue;
        }
        if (state[ch] != CH_STALE && state[ch] != CH_BARE) {
            continue;
        }
        if (fails[ch] < 0) {
            fails[ch] = boot_fail_count(ch, ver);
        }
        if (fails[ch] >= BOOT_FAIL_MAX) {
            ESP_LOGE(TAG, "  ch%u: %d failed auto-flash attempts for v%s — giving up "
                          "on this target; run: ambit_flash %u %s",
                     ch, fails[ch], ver, ch, ver);
            state[ch] = CH_SKIPPED;
            n_skipped++;
            continue;
        }
        n_need++;
    }
    if (n_need == 0) {
        if (n_skipped > 0) {
            ESP_LOGW(TAG, "boot sync: nothing to auto-flash, but %d channel(s) were "
                          "SKIPPED (fail cap / unreadable version — see lines above)",
                     n_skipped);
        } else {
            ESP_LOGW(TAG, "boot sync: all present AMBITs match v%s — nothing to flash", ver);
        }
        return 0;
    }

    /* Power gate — the same condition that gates MQTT publishing. The gate needs
     * ~15 s of observed external power to open from its boot-closed state, so
     * give it time instead of sampling once. */
    if (!device_commands_publish_power_ok()) {
        ESP_LOGW(TAG, "boot sync: %d channel(s) need flashing — waiting up to %d s "
                      "for the power gate", n_need, BOOT_GATE_WAIT_MS / 1000);
        for (int waited = 0;
             waited < BOOT_GATE_WAIT_MS && !device_commands_publish_power_ok();
             waited += BOOT_GATE_POLL_MS) {
            vTaskDelay(pdMS_TO_TICKS(BOOT_GATE_POLL_MS));
        }
    }
    if (!device_commands_publish_power_ok()) {
        ESP_LOGE(TAG, "boot sync: on battery — %d channel(s) stay on their current "
                      "firmware until a powered boot (or run ambit_flash manually)", n_need);
        return 0;
    }

    /* Flash + verify each channel. ~30-60 s per channel; the console is already
     * up and Lua has not started, so the UART bus is exclusively ours. */
    int flashed = 0;
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        if (state[ch] != CH_STALE && state[ch] != CH_BARE) {
            continue;
        }
        ESP_LOGW(TAG, "boot sync: auto-flashing ch%u -> v%s", ch, ver);
        ambit_flash_image_result_t res;
        bool verified = false;
        if (ambit_flash_image(ch, tgt.dir, 0, &res) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(BOOT_VERIFY_DELAY_MS));
            for (int try = 0; try < BOOT_VERIFY_TRIES && !verified; try++) {
                ambit_fw_info_t fw;
                if (boot_read_version(ch, &fw)) {
                    if (fw_matches(&fw, &tgt)) {
                        verified = true;
                    } else {
                        ESP_LOGE(TAG, "  ch%u: flashed OK but app reports v%u.%u.%u — "
                                      "does the folder name match the image?",
                                 ch, fw.major, fw.minor, fw.batch);
                        break;
                    }
                } else if (try + 1 < BOOT_VERIFY_TRIES) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            }
        }
        if (verified) {
            boot_fail_clear(ch);
            flashed++;
            ESP_LOGW(TAG, "  ch%u: now running v%s (verified)", ch, ver);
        } else {
            boot_fail_bump(ch, ver, fails[ch]);
            ESP_LOGE(TAG, "  ch%u: auto-flash NOT verified (attempt %d/%d for v%s)",
                     ch, fails[ch] + 1, BOOT_FAIL_MAX, ver);
        }
    }
    ESP_LOGW(TAG, "boot sync: %d/%d channel(s) flashed to v%s", flashed, n_need, ver);
    return flashed;
}
