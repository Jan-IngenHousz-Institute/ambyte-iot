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
 *   ota_update    refused at worker entry, at final admission (after the ack
 *                 flush and fleet spread, before suspend/download) and before
 *                 the image is selected: ota_status "dropped", detail
 *                 "power_protect", Jobs retryable.
 *   ambit_ota     ambit_ota / ambit_flash refused at entry, after the ack
 *                 flush / spread, after the download, and before EACH
 *                 channel's write (a started write finishes); attach updates
 *                 held (re-checked after the quiesce).
 *   ambit_flash   boot sync does not flash (re-checked before each channel).
 * Every update path re-checks at its safe boundaries because protection can
 * begin while a worker waits (review of PR #103).
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
    bool        persist_fault;  /* NVS copy unreadable at boot or last write failed */
    uint32_t    brownouts_6h;
    pp_skips_t  skipped;
    int64_t     exit_in_s;      /* qualifying seconds still needed to exit; -1 = none collected */
    pp_recovery_t recovery;     /* accumulated evidence, probe, backoff level */
} power_protect_status_t;

/* Once per boot, after nvs_flash_init + diag_boot_init and after the RTC /
 * clock_trust bootstrap, BEFORE AMBIT boot sync and the schedule runner start
 * (so the first action after a brownout is already shed). Never fails boot:
 * an unreadable blob starts at no_flash, state_unreadable (logged). */
void power_protect_boot(void);

/* Feed one charger read (device_commands' power-state refresh, after
 * power_gate_step). `fresh_present` = power_gate_flash_ok(). This is also
 * where the accumulated exit, the battery-only probe and the probation are
 * decided: without charger reads nothing of that runs (fail-closed). */
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

/* A relaxation could not be shown to have landed, and the protective fence
 * written over it could not be confirmed on NVS either: the unit is protected
 * now and across resets without power loss, but after a power loss the
 * relaxation may still win. (Command reply detail "persist_unconfirmed".) */
#define POWER_PROTECT_ERR_UNCONFIRMED ESP_ERR_NOT_FINISHED

/* Operator actions; the return value is the NVS persist result (ESP_OK also
 * when nothing needed writing). clear and a force DOWN (hold -> no_flash) are
 * relaxations: written first, and NOT applied when the write fails. After a
 * write error only a positive read of the new record counts as success;
 * anything else is fenced by a newer protective record and the result is an
 * error (POWER_PROTECT_ERR_UNCONFIRMED when the fence is not confirmed on NVS
 * either). A force
 * UP applies at once; if NVS refuses it, it holds in RAM and the RTC mirror
 * (resets without power loss), not across a power loss. */
esp_err_t power_protect_force(pp_state_t state);
esp_err_t power_protect_clear(void);

#ifdef __cplusplus
}
#endif
