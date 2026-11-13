#pragma once

/* Attach-time AMBIT firmware gate: an AMBIT is measured only after its firmware
 * was checked against the SD target (/sdcard/ambit_fw/<ver>/) and, if older,
 * updated.
 *
 * Why: boot sync (ambit_flash_boot_sync) brings every AMBIT present AT BOOT up to
 * the SD target, but an AMBIT plugged in while the Ambyte runs was measured with
 * whatever firmware it carried until the next clean, externally powered boot,
 * which on a solar unit (nightly reboot ~01:20 UTC, no input) may never come.
 *
 * The schedule asks ambit_attach_gate_allow() for every channel it is about to
 * measure (sched_runner ch_present). The gate remembers, per channel, the
 * (MAC, firmware) pair the checker last CLEARED. Any other pair, so a new
 * AMBIT, a swapped one, or one just reflashed, is HELD (not measured) and the
 * checker is woken; the checker (ambit_ota worker) reports a verdict:
 *   CLEAR     current, newer than the target, no target on SD, or not
 *             auto-updatable: measure it;
 *   CLEAR_RECHECK  target unknown right now (SD unavailable/parked): measure,
 *             re-check later, a parked card must not stop measurement;
 *   RETRY     out of date but power too low to flash: stay held, re-check later;
 *   FLASHED   updated: the identity changes, so the next gate call checks the
 *             new firmware (normally CLEAR) before measuring it.
 * An AMBIT whose identity cannot be read (pre-1.0 units that do not answer the
 * version read) is not held: it can never be auto-updated, and holding it would
 * silence the channel for good.
 *
 * The core is pure (no RTOS, no I/O): tests/test_ambit_attach.py compiles it. */

#include <stdbool.h>
#include <stdint.h>

#include "sensing_port.h"   /* power_reading_t */

#ifdef __cplusplus
extern "C" {
#endif

#define AMBIT_ATTACH_CHANNELS   4u
#define AMBIT_ATTACH_RETRY_MS   (10u * 60u * 1000u)   /* low power / unknown target */

typedef enum {
    AMBIT_ATTACH_CLEAR = 0,
    AMBIT_ATTACH_CLEAR_RECHECK,
    AMBIT_ATTACH_RETRY,
    AMBIT_ATTACH_FLASHED,
} ambit_attach_verdict_t;

/* Wakes the checker; returns false when it could not be queued (it is retried on
 * the next gate call). Registered by app_main (ambit_ota_request_attach_check). */
typedef bool (*ambit_attach_kick_fn)(void);

void ambit_attach_gate_set_kick(ambit_attach_kick_fn kick);

/* Gate decision for channel `ch` whose AMBIT currently identifies as
 * (`device_id`, `fw`). device_id NULL = identity unreadable (never held). */
bool ambit_attach_gate_allow(uint8_t ch, const char *device_id, const char *fw, int64_t now_ms);

/* Checker side: channels waiting for a check (bit per channel). The checker
 * reports each result for the identity it actually checked. */
uint8_t ambit_attach_gate_pending(void);
void    ambit_attach_gate_resolve(uint8_t ch, const char *device_id, const char *fw,
                                  ambit_attach_verdict_t verdict, int64_t now_ms);

/* The checker finished a pass (queued again on the next gate call if needed). */
void    ambit_attach_gate_checker_done(void);

/* True while `ch` is held (diagnostics). */
bool    ambit_attach_gate_held(uint8_t ch);

/* Power for an attach-time flash is device_commands_flash_power_ok() (fresh,
 * positive evidence of external power; fail-closed). The former battery-floor
 * rule (AMBIT_ATTACH_BATTERY_FLOOR_MV, judged on whatever frame the MP2731 held)
 * was retired on 2026-11-13: on battery that frame is stale, and the decision is
 * no AMBIT flashing on battery or unknown power. */

/* Forget everything (tests; also a port that came up empty). */
void    ambit_attach_gate_forget(uint8_t ch);

#ifdef __cplusplus
}
#endif
