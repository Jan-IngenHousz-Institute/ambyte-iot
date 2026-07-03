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

#ifdef __cplusplus
}
#endif
