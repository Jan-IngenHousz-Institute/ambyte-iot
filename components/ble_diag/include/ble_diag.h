#pragma once

/* Bench-only read-only diagnostics over BLE (env bench-ble, contract v0 in
 * docs/bench/ble-diagnostics.md). NOT for release: no authentication, and the
 * radio's power and coexistence cost is unqualified. Every release env builds
 * this component empty (CONFIG_AMBYTE_BLE_DIAG depends on BT, which is off). */

#include <stddef.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_AMBYTE_BLE_DIAG
/* Allocate the snapshot buffers (PSRAM when present). No radio. Call before
 * device_commands_init so the first heartbeat is captured. */
esp_err_t ble_diag_init(void);
/* device_commands_config_t.telemetry_built: copy the heartbeat sample object
 * the firmware just built into the served snapshot (seq++). Never reads a
 * sensor; blocks at most 100 ms on the snapshot writer mutex, then drops. */
void ble_diag_snapshot_set(const char *json, size_t len);
/* Start NimBLE, the GATT service, the 15-min advertising window, the worker
 * task and the `ble` console command. Call after the console is up. */
esp_err_t ble_diag_start(void);
#endif

#ifdef __cplusplus
}
#endif
