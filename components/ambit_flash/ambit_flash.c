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

#include "driver/uart.h"
#include "esp_log.h"

#include "esp_loader.h"
#include "uart_sensors.h"
#include "sd_card.h"

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
