#pragma once
/*
 * power_protect.h — brownout protection, the device side (2026-11).
 *
 * The decisions are power_protect_core.h (pure, host-tested). This is the
 * glue: the persisted state in NVS (namespace "pwr_protect", one blob, written
 * only on transitions and brownout records), the boot evaluation, the
 * transition log line, the skip counters, and a lock-free state word every
 * consumer reads.
 *
 * Consumers and what they shed (docs/power-protect.md):
 *   sched_runner  run_job: a job with actinic-bearing AMBIT work is skipped in
 *                 no_flash, every job with an ambit/ step in hold. A skip is
 *                 not a failure (no fail streak, no failure log).
 *   ota_update    refused before any download, ota_status "dropped",
 *                 detail "power_protect" (a Jobs execution stays QUEUED).
 *   ambit_ota     ambit_ota / ambit_flash refused, detail "power_protect";
 *                 attach updates held.
 *   ambit_flash   boot sync does not flash.
 *   device_commands_flash_power_ok() is false (defence in depth).
 * Never touched: the event log, its cursor, the delivery pipeline, MQTT,
 * script_update, the heartbeat, log_list/log_read.
 *
 * Threads: power_protect_state() and the blocks_* probes are lock-free (safe
 * on the esp-mqtt task). Everything else takes a FreeRTOS mutex and may write
 * NVS; it is called from the charger-read path, the maintenance worker and the
 * heartbeat task, never from the esp-mqtt task.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "power_protect_core.h"
#include "sensing_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POWER_PROTECT_SKIP_FLASH_RUN = 0,
    POWER_PROTECT_SKIP_AMBIT_RUN,
    POWER_PROTECT_SKIP_OTA,
} power_protect_skip_t;

typedef struct {
    pp_state_t  state;
    pp_reason_t reason;
    bool        forced;
    int64_t     since_utc;      /* 0 = unknown (clock was not valid) */
    uint32_t    brownouts_6h;
    pp_skips_t  skipped;
    int64_t     exit_in_s;      /* seconds left in a running exit window; -1 = none */
} power_protect_status_t;

/* Once per boot, after nvs_flash_init + diag_boot_init and after the RTC /
 * clock_trust bootstrap, BEFORE AMBIT boot sync and the schedule runner start
 * (so the first action after a brownout is already shed). Never fails boot:
 * an unreadable blob starts at normal (logged). */
void power_protect_boot(void);

/* Feed one charger read (device_commands' power-state refresh, after
 * power_gate_step). `fresh_present` = power_gate_flash_ok(). */
void power_protect_on_power_read(power_read_reason_t reason, const power_reading_t *p,
                                 bool fresh_present);

/* Lock-free. Before power_protect_boot: normal. */
pp_state_t power_protect_state(void);
bool power_protect_blocks_flash(void);   /* no_flash or hold */
bool power_protect_blocks_ambit(void);   /* hold */

void power_protect_note_skip(power_protect_skip_t what);

void power_protect_get_status(power_protect_status_t *out);

/* health.power_protect. Returns the length, -1 when it does not fit. */
int power_protect_render_json(char *buf, size_t cap);

/* Operator actions. The new state applies in RAM at once; the return value is
 * the NVS persist result (ESP_OK also when nothing needed writing). */
esp_err_t power_protect_force(pp_state_t state);
esp_err_t power_protect_clear(void);

#ifdef __cplusplus
}
#endif
