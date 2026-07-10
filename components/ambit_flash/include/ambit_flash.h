#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of ambit_flash_probe(). */
typedef struct {
    bool    connected;   /* true = the AMBIT ROM bootloader answered SYNC */
    int     chip;        /* target_chip_t; 3 = ESP32-C3 / ESP8685 */
    uint8_t mac[6];      /* efuse MAC read from the ROM (best-effort) */
} ambit_flash_probe_result_t;

/*
 * Host-driven ROM flasher for the AMBIT (ESP8685 = ESP32-C3) over the shared
 * FFC/UART, via esp-serial-flasher with a custom port that reuses uart_sensors'
 * UART0 + the reset/boot sequencer.
 *
 * ambit_flash_probe: force the AMBIT on `channel` into ROM serial download mode,
 * connect via the ROM bootloader, read its chip type + MAC, then reset it back to
 * its application. Proves the end-to-end flasher path with no external hardware.
 *
 * Takes the shared UART bus for the duration (quiesce Lua first — a busy bus
 * returns ESP_ERR_TIMEOUT). `out` may be NULL. Returns ESP_OK only when the ROM
 * answered.
 */
esp_err_t ambit_flash_probe(uint8_t channel, ambit_flash_probe_result_t *out);

/* Result of ambit_flash_image(). */
typedef struct {
    bool     ok;
    int      chip;             /* detected target_chip_t (3 = C3/ESP8685) */
    int      regions_written;  /* how many of the 4 regions completed */
    uint32_t total_bytes;      /* total bytes written across regions */
} ambit_flash_image_result_t;

/*
 * Full ROM flash of the AMBIT (ESP8685/C3) on `channel` from an SD directory that
 * holds the four region images produced by the AMBIT build:
 *     bootloader.bin -> 0x0
 *     partitions.bin -> 0x8000
 *     boot_app0.bin  -> 0xe000   (otadata)
 *     app.bin        -> 0x10000
 * Streams each file from SD in blocks, MD5-verifies per region, then resets the
 * AMBIT into the new app. NVS (0x9000) is never written, so per-unit calibration
 * survives. Works on bare / bricked / any-firmware units (ROM bootloader path).
 *
 * `baud` = link speed after the stub connect (0 => default). Takes the shared UART
 * bus for the whole operation — quiesce Lua first (a busy bus returns ESP_ERR_TIMEOUT).
 * `out` may be NULL. Returns ESP_OK only when all four regions verified.
 */
esp_err_t ambit_flash_image(uint8_t channel, const char *dir, uint32_t baud,
                            ambit_flash_image_result_t *out);

/* A firmware image discovered on the SD card: its version + directory. */
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t batch;
    char    dir[96];   /* e.g. "/sdcard/ambit_fw/0.0.7" */
} ambit_flash_target_t;

/* Scan /sdcard/ambit_fw for version-named subfolders (<major>.<minor>.<batch>)
 * that contain all four region files, and return the HIGHEST version as the
 * target. ESP_OK + *out on success; ESP_ERR_NOT_FOUND if none present;
 * ESP_ERR_INVALID_STATE if the SD is unavailable. */
esp_err_t ambit_flash_find_target(ambit_flash_target_t *out);

/* Detect + report (Phase 3, no flashing): find the SD target image, then for each
 * present AMBIT read its running version (cmd 33/2) and log whether it matches the
 * target — including the exact `ambit_flash <ch> <ver>` to run on a mismatch.
 * Returns the number of present channels that differ from the target, or -1 if no
 * SD target image is present. Read-only and bus-mutex-serialised, so it is safe to
 * run with the Lua measurement loop active. */
int ambit_flash_check(void);

/* Boot-time firmware sync (Phase 3 full): find the SD target image, read each
 * present AMBIT's running version (ROM-probing silent channels so bare/bricked
 * units are found too), then flash every channel whose version differs from the
 * target — automatically, no operator. Flashing is gated on the same power
 * condition as MQTT publishing (device_commands_publish_power_ok), and a
 * per-channel NVS fail counter skips a channel after repeated failed attempts
 * for the same target so a broken unit can't add a doomed flash to every boot.
 *
 * MUST run before the Lua measurement loop starts (it takes the shared UART bus
 * without any quiesce dance, and probing hard-resets all four AMBITs via the
 * shared enable line). Returns the number of channels flashed AND verified at
 * the target version, or -1 if no SD target image is present. */
int ambit_flash_boot_sync(void);

#ifdef __cplusplus
}
#endif
