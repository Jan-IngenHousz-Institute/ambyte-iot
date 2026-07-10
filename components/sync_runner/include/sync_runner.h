#pragma once

#include <stdbool.h>
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
 * sync_runner_is_allowed() holds: one in-flight slot at a time (see
 * device_commands.c). Registers its wake hook via device_commands_set_sync_notifier().
 * Idempotent; subsequent calls are no-ops.
 *
 * Power gating: the drain only runs while sync_runner_is_allowed() is true
 * (no measurement in progress AND device on external power). Events otherwise
 * stay PENDING and drain when the gate reopens (caught by the fallback timer).
 * Additionally, publishing is gated while the system clock is implausible
 * (pre-2024) so 1970-stamped events never reach the cloud.
 *
 * @param heartbeat_s STATUS heartbeat period in seconds (stores one tag=STATUS
 *        event per period via cmd_store_status_event, first one immediately);
 *        0 disables the heartbeat. Resolution = the 30 s fallback wake.
 */
esp_err_t sync_runner_start(uint32_t heartbeat_s);

/**
 * @brief Wake the sync task to (re)evaluate the drain. Safe before start
 *        (no-op until the task exists). Registered as the store/measurement-end
 *        notifier so publishing is event-driven, not polled.
 */
void sync_runner_notify(void);

/**
 * @brief Gate hook for the power-aware policy. Weak; returns true only when no
 *        measurement is active AND the device is on external power.
 */
bool sync_runner_is_allowed(void);

/**
 * @brief Report the connectivity-watchdog inputs and verdict (using the real
 *        timeout). Any out-pointer may be NULL. Returns true if the device is
 *        currently in the reboot-warranting state (external power, clock valid,
 *        events pending, and no PUBACK within the timeout). NOTE: *allowed is
 *        the POWER gate only — the watchdog deliberately ignores the measurement
 *        window (a near-100%-duty schedule phase-locked to the 60 s tick blinded
 *        it in the field; the drain's own gate still honours the window).
 */
bool sync_runner_watchdog_status(bool *allowed, bool *clock_ok, int64_t *pending,
                                 int64_t *since_ms, int64_t *timeout_ms);

/**
 * @brief Test hook: force the watchdog to evaluate immediately with a zero
 *        timeout. If publishing is allowed with pending work, the device reboots
 *        now — used to validate the watchdog on hardware without waiting an hour.
 */
void sync_runner_watchdog_test(void);

#ifdef __cplusplus
}
#endif
