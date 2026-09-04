#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the background MQTT sync task — the sole MQTT publisher.
 *
 * Blocks until woken (a measurement event was stored, or a measurement burst
 * finished) or a fallback timeout fires, then drains all pending events while
 * sync_runner_is_allowed() holds: up to the configured slot/byte publish window
 * may await PUBACK concurrently (see device_commands.c). Registers its wake hook
 * via device_commands_set_sync_notifier().
 * Idempotent; subsequent calls are no-ops.
 *
 * Power gating: the drain only runs while sync_runner_is_allowed() is true
 * (no raw sensor transaction in progress AND device on external power). Events
 * otherwise stay PENDING and drain when the gate reopens. A compile-time legacy
 * switch restores the former whole-measurement gate.
 * Additionally, publishing is gated while the system clock is implausible
 * (pre-2024) so 1970-stamped events never reach the cloud.
 *
 * @param heartbeat_s STATUS heartbeat period in seconds (stores one tag=STATUS
 *        event per period from the independent watchdog task via
 *        cmd_store_status_event, first one immediately after app boot-complete);
 *        0 disables the heartbeat. Resolution = the watchdog's 60 s tick.
 */
esp_err_t sync_runner_start(uint32_t heartbeat_s);

/** Configure the app-owned global maintenance-state probe. NULL means no
 * maintenance is active. Set once before sync_runner_start(); every automatic
 * recovery path vetoes reboot while the probe returns true. */
void sync_runner_set_maintenance_probe(bool (*probe)(void));

/**
 * @brief Wake the sync task to (re)evaluate the drain. Safe before start
 *        (no-op until the task exists). Registered as the store/gate-end notifier
 *        so publishing is event-driven, not polled.
 */
void sync_runner_notify(void);

/**
 * @brief Signal that app_main's startup sequence is complete. The first drain
 *        pass is held until this fires (plus the usual stagger) so boot-time
 *        backlog traffic can never compete with console/schedule bring-up.
 *        Falls open on its own after a generous timeout if never called.
 */
void sync_runner_boot_complete(void);

/**
 * @brief Gate hook for the power-aware policy. Weak; returns true only when no
 *        raw sensor publish hold is active AND the device is on external power
 *        (or, in legacy mode, when no measurement is active).
 */
bool sync_runner_is_allowed(void);

/**
 * @brief Report the connectivity-watchdog inputs and verdict (using the real
 *        timeout). Any out-pointer may be NULL. Returns true if the device is
 *        currently in the reboot-warranting state (external power, clock valid,
 *        events pending, and no PUBACK within the timeout). NOTE: *allowed is
 *        the POWER gate only — the watchdog deliberately ignores both the narrow
 *        raw-sensor hold and the legacy measurement window. A near-100%-duty
 *        schedule phase-locked to the 60 s tick blinded the old implementation
 *        in the field.
 */
bool sync_runner_watchdog_status(bool *allowed, bool *clock_ok, int64_t *pending,
                                 int64_t *since_ms, int64_t *timeout_ms);

/**
 * @brief Test hook: force the watchdog to evaluate immediately with a zero
 *        timeout. If publishing is allowed with pending work, the device reboots
 *        now — used to validate the watchdog on hardware without waiting an hour.
 */
void sync_runner_watchdog_test(void);

/** Read this boot's persistent self-healing reboot reason ("nightly", "conn",
 * "mem", or "nopuback"). The stored reason carries its expected boot epoch;
 * ESP_ERR_NVS_NOT_FOUND also means the latest reason belongs to an older boot.
 * Intended for STATUS telemetry composition without stale-cause leakage. */
esp_err_t sync_runner_get_last_wd_reboot_reason(char *out, size_t out_cap);

/** Return the persistent generation assigned to the current boot. Exposed so
 * telemetry/tests can correlate the boot-scoped watchdog reason explicitly. */
esp_err_t sync_runner_get_boot_epoch(uint32_t *out);

/** True only after the watchdog task was created and this boot's persistent
 * generation was committed successfully. False means self-healing is not
 * safely armed for this boot (STATUS still may be emitted by a running task). */
bool sync_runner_watchdog_armed(void);

#ifdef __cplusplus
}
#endif
