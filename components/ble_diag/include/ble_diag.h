#pragma once

/* Bench-only read-only diagnostics over BLE (env bench-ble, contract v0 in
 * docs/bench/ble-diagnostics.md). NOT for release: no authentication, and the
 * radio's power and coexistence cost is unqualified. Every release env builds
 * this component empty (CONFIG_AMBYTE_BLE_DIAG depends on BT, which is off). */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_AMBYTE_BLE_DIAG
/* Allocate the snapshot buffers (PSRAM when present), then initialise the BT
 * controller, NimBLE host, GATT table, host task and worker task. Nothing is
 * transmitted until ble_diag_start() opens the window. Call EARLY in boot, before Wi-Fi/TLS fragment the internal
 * heap (the controller needs internal blocks the post-TLS heap cannot supply).
 * An error from the stack leaves the snapshot usable (ble_diag_snapshot_ready). */
esp_err_t ble_diag_init(void);
/* True once the snapshot buffers exist (the telemetry_built hook may be wired). */
bool ble_diag_snapshot_ready(void);
/* device_commands_config_t.telemetry_built: copy the heartbeat sample object
 * the firmware just built into the served snapshot (seq++). Never reads a
 * sensor; blocks at most 100 ms on the snapshot writer mutex, then drops. */
void ble_diag_snapshot_set(const char *json, size_t len);
/* Open the 15-min advertising window and register the `ble` console command
 * (the host and worker tasks already run, silent, since ble_diag_init). Call
 * after the console is up. */
esp_err_t ble_diag_start(void);
#endif

#ifdef __cplusplus
}
#endif
