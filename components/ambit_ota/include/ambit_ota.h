#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "messaging_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-driven AMBIT (ESP32-C3) firmware update over UART.
 *
 * The ambyte downloads a C3 firmware image from a public HTTPS URL to the SD
 * card, then streams it to one AMBIT sensor over the existing binary UART link
 * (cmds 25-28: OTA_BEGIN / OTA_DATA / OTA_END / OTA_ABORT — see run_esp.cpp on
 * the ambit side and cmd_ambit_ota_* in device_commands). The AMBIT writes the
 * image into its spare OTA slot via Arduino's Update, verifies it, and reboots
 * into it. The ambyte itself does NOT reboot.
 *
 * Quiescing: the worker stops the Lua measurement task (so it can't contend for
 * the shared UART) and MQTT (the board can't hold two TLS sessions during the
 * download) for the duration, then resumes both. The image is staged on SD
 * first — not streamed straight from HTTP — because the C3 sleeps after UART
 * silence and the heap can't buffer a whole image; SD decouples the two halves.
 *
 * v1 is CLI-triggered (`ambit_ota <ch> <url>`); an MQTT trigger is a follow-up.
 */

/* channel value meaning "every responding AMBIT (0..3), in turn". */
#define AMBIT_OTA_CH_ALL  0xFFu

typedef struct {
    void (*workload_suspend)(void);   /* stop the Lua task during the update; NULL = skip */
    void (*workload_resume)(void);    /* restart it afterward; NULL = skip */
    void (*comms_suspend)(void);      /* mqtt_client_stop — free TLS heap for the HTTPS download */
    void (*comms_resume)(void);       /* mqtt_client_start — after the update */

    /* Optional best-effort status reporting (used once MQTT is back; NULL = log-only). */
    message_publish_fn      publish;
    message_is_connected_fn is_connected;
    const char             *status_topic;
    const char             *device_id;
} ambit_ota_config_t;

/* Start the AMBIT-OTA worker task (idempotent). */
esp_err_t ambit_ota_init(const ambit_ota_config_t *cfg);

/* Queue an AMBIT firmware update on `channel` (0-3) from `url` (a direct .bin —
 * raw.githubusercontent.com/… or a release-asset link, NOT a /blob//tree/ page).
 * `id` correlates status reports and dedupes a retained MQTT trigger (latched
 * only on success, so a failed attempt retries under the same id); pass NULL
 * from the CLI to never dedupe. Non-blocking: the worker quiesces, downloads,
 * streams, and the AMBIT reboots. ESP_ERR_INVALID_STATE before init;
 * ESP_ERR_INVALID_ARG on a bad channel/url; ESP_ERR_NO_MEM if one is in flight. */
esp_err_t ambit_ota_request(uint8_t channel, const char *url, const char *id);

/* Queue a fleet version sweep: read each channel's AMBIT firmware version (cmd
 * 33/2) and publish one `ambit_versions` JSON report (+ log per channel). Runs
 * on the worker task (off the MQTT loop). `id` correlates the report; NULL ok.
 * The CLI reads versions directly instead (synchronous console output). */
esp_err_t ambit_ota_report_versions(const char *id);

/* Queue a ROM-bootloader probe of `channel` (0-3, or AMBIT_OTA_CH_ALL): drive the
 * straps into download mode, read chip + MAC, reset back to the app, and publish
 * one `ambit_probe` JSON report (per channel: rom+chip+mac, rom:false, or
 * error:"bus_busy" when Lua wouldn't release the UART — indeterminate, NOT
 * absent). Works regardless of the AMBIT's app firmware (bricked/blank units
 * answer too) — the remote twin of the CLI `ambit_probe`.
 *
 * NOT harmless to measurements: the reset strap (CHIP_EN) is SHARED, so every
 * probe hard-resets ALL FOUR AMBITs and aborts any in-progress sensor run on
 * every channel. Lua is stopped for the (few-second) window; MQTT stays up.
 * Never deduped (idempotent identity read); `id` only correlates the report. */
esp_err_t ambit_ota_request_probe(uint8_t channel, const char *id);

/* Queue a full 4-region ROM flash from /sdcard/ambit_fw/<version>/ onto `channel`
 * (0-3, or AMBIT_OTA_CH_ALL = every channel whose ROM answers a probe — including
 * units whose app firmware is dead, which ambit_ota cannot reach). `version` must
 * be canonical <major>.<minor>.<batch> (path-safe; each part 0-255). NVS@0x9000 is
 * never written, so per-unit calibration survives. Quiesces Lua + MQTT for the
 * whole sweep (~10-60 s per channel), resumes, then publishes per-channel
 * `ambit_flash_status` reports (flash_ok / flash_failed / absent / bus_busy).
 * `id` dedupes a retained trigger — latched (own NVS key, separate from the OTA
 * latch) only when every ROM-answering channel flashed OK; NULL (CLI) never
 * dedupes. A persistently-FAILING id is refused after 3 attempts (returns
 * ESP_ERR_INVALID_STATE) so a retained trigger with a bad version/unstaged SD
 * can't loop the disruptive sweep forever — fix the cause, retry under a new id.
 * The SD folder must already hold the 4 region files — this path does not
 * download. */
esp_err_t ambit_ota_request_flash(uint8_t channel, const char *version, const char *id);

#ifdef __cplusplus
}
#endif
