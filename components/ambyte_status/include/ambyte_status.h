#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "device_status_port.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ambyte_status_init(void);
esp_err_t ambyte_status_set_rgb(uint8_t r, uint8_t g, uint8_t b);
status_set_fn ambyte_status_get_set_fn(void);

/* ── Field-status blinker ──────────────────────────────────────────────
 * Firmware-owned LED state indicator (the schedule runner does not drive the
 * LED). One short flash per cycle; colour encodes state, cadence/brightness
 * follow the power source. State probes are sampled every cycle; any may be
 * NULL (defaults: sd=true, wifi=false, provisioned=false, script=false,
 * external=true, battery unknown). */
typedef struct {
    bool     (*sd_mounted)(void);
    bool     (*wifi_connected)(void);
    bool     (*provisioned)(void);
    bool     (*script_running)(void);     /* schedule task alive             */
    bool     (*on_external_power)(void);  /* debounced VIN present           */
    uint32_t (*battery_mv)(void);         /* last known mV; 0 = unknown      */
} ambyte_blinker_config_t;

/* Start the blinker task (static allocation; call once, after
 * ambyte_status_init). Colours, priority order:
 *   red          SD not mounted (fault — always 3 s, full brightness)
 *   red ×2       on battery below 3.5 V (double-flash, dim, slow)
 *   purple       not provisioned
 *   green/blue   script running, Wi-Fi up / down
 *   white/yellow idle (script done or never started), Wi-Fi up / down
 * Cadence: 3 s full brightness on external power; 15 s at 1/8 on battery. */
esp_err_t ambyte_status_blinker_start(const ambyte_blinker_config_t *cfg);

#ifdef __cplusplus
}
#endif
