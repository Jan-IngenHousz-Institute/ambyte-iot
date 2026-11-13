#ifndef AMBYTE_POWER_POLICY_H
#define AMBYTE_POWER_POLICY_H

/* What the firmware DOES with an MP2731 power reading - the publish gate and the
 * low-battery SD park guard - as pure state machines over (reading, verdict,
 * now). No I2C, no clock, no logging: device_commands.c and app_main.c own those
 * and feed every sample through here, so tests/power_policy_host.c can drive the
 * exact production policy with a fake charger (stuck, all-zero, never-completing,
 * erroring, healthy).
 *
 * THE RULE (field incident 2026-11-13, Uluru): a power reading is evidence. An
 * invalid reading (power_read_reason_t != OK) is the ABSENCE of evidence, and the
 * absence of evidence must never hold field data back:
 *   - it never CLOSES the publish gate and never PARKS the SD;
 *   - it never KEEPS either closed or parked: after POWER_POLICY_INVALID_FAILOPEN_N
 *     consecutive invalid samples the gate opens ("power_unverified") and a parked
 *     SD is remounted.
 * 14:63:93:37:A3:FC read an all-zero frame for 9+ h; the old gate took the frame at
 * face value ("no input"), stayed closed, and held 2,519 records back. Valid
 * readings behave exactly as before this module existed (same dwell times, same
 * thresholds). The one consumer that stays fail-CLOSED is AMBIT flashing - a
 * brownout mid-flash bricks a sensor, so it needs POSITIVE evidence of power
 * (power_gate_flash_ok). The trade-off of failing open is battery: a unit whose
 * charger cannot be read publishes on battery too, which can drain it faster at
 * night. That is the cheaper failure - internal littlefs keeps measuring through
 * a brownout, but a closed gate on a dead charger reading holds data back for as
 * long as the reading stays dead (forever, on a frozen chip), until the ~17 h
 * store budget overflows. See docs/power-telemetry.md. */

#include <stdbool.h>
#include <stdint.h>

#include "sensing_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Consecutive invalid samples before the fail-open fallback engages. The gate
 * samples at most every 5 s (PUB_GATE_EVAL_INTERVAL_MS) and the SD guard every
 * 15 s, so 12 samples = ≥60 s of gate evaluations / 3 min of guard polls: long
 * enough that one I2C glitch or one slow conversion never flips anything, short
 * enough that a frozen charger costs minutes of delivery, not hours. */
#define POWER_POLICY_INVALID_FAILOPEN_N 12u

/* Publish-gate dwell (unchanged from Phase 1): power present this long opens the
 * gate, absent this long closes it. */
#define POWER_GATE_ON_DWELL_MS  15000
#define POWER_GATE_OFF_DWELL_MS 60000

/* Why the gate is in its current state (telemetry `publish_gate_reason`). */
typedef enum {
    POWER_GATE_REASON_STARTING = 0,  /* closed: boot, no decisive evidence yet */
    POWER_GATE_REASON_EXTERNAL_POWER, /* open: valid reading, external power */
    POWER_GATE_REASON_ON_BATTERY,     /* closed: valid reading, no external power */
    POWER_GATE_REASON_POWER_UNVERIFIED, /* open: fail-open after N invalid reads */
    POWER_GATE_REASON_NO_CHARGER,     /* open: no charger wired (never gates) */
} power_gate_reason_t;

const char *power_gate_reason_name(power_gate_reason_t r);

typedef struct {
    bool     open;            /* debounced gate state (closed at boot) */
    bool     cand;            /* last valid instantaneous reading vs. gate */
    int64_t  cand_ms;         /* when cand first differed from the gate */
    uint32_t invalid_streak;  /* consecutive invalid samples */
    bool     last_valid;      /* the most recent sample was valid */
    int64_t  verify_ms;       /* fail-open → external_power upgrade timer (0 = idle) */
    power_gate_reason_t reason;
} power_gate_t;

/* What one sample did, for the caller's (rate-limited) logging. */
typedef enum {
    POWER_GATE_STEP_NONE = 0,
    POWER_GATE_STEP_OPENED,          /* valid evidence opened it */
    POWER_GATE_STEP_CLOSED,          /* valid evidence closed it */
    POWER_GATE_STEP_FAILOPEN,        /* N invalid samples opened it */
} power_gate_step_t;

void power_gate_init(power_gate_t *g);

/* Feed one sample. `reason` is the read verdict; `present` is only consulted when
 * reason == POWER_READ_OK. */
power_gate_step_t power_gate_step(power_gate_t *g, int64_t now_ms,
                                  power_read_reason_t reason, bool present);

/* AMBIT flashing: positive evidence only. True iff the gate is open on a VALID
 * external-power decision and the latest sample was itself valid - never on the
 * fail-open fallback (a frozen charger is exactly when we cannot know whether the
 * battery will survive a 30–60 s flash). */
bool power_gate_flash_ok(const power_gate_t *g);

/* ── Low-battery SD park guard ───────────────────────────────────────────── */

#define POWER_GUARD_PARK_MV   3300u  /* on-battery floor: park the SD below this */
#define POWER_GUARD_PARK_N       3u  /* consecutive low reads (45 s @ 15 s) */
#define POWER_GUARD_RESUME_MV 3600u  /* battery-only resume level (> park + rebound) */
#define POWER_GUARD_RESUME_N     4u  /* consecutive good reads (60 s) before un-parking */

typedef struct {
    bool     parked;
    uint32_t low_n;
    uint32_t good_n;
    uint32_t invalid_n;   /* consecutive samples carrying no battery evidence */
} power_guard_t;

typedef enum {
    POWER_GUARD_HOLD = 0,
    POWER_GUARD_PARK,               /* valid low-battery evidence: park the SD */
    POWER_GUARD_UNPARK,             /* valid recovery evidence: remount */
    POWER_GUARD_UNPARK_UNVERIFIED,  /* fail-open: N samples without evidence */
} power_guard_action_t;

void power_guard_init(power_guard_t *g);

/* Feed one sample. `p` is consulted only when reason == POWER_READ_OK.
 * `maintenance_active` vetoes a PARK (never an un-park). The caller performs the
 * action and the guard has already moved to the new state when it returns
 * PARK/UNPARK*. */
power_guard_action_t power_guard_step(power_guard_t *g, power_read_reason_t reason,
                                      const power_reading_t *p, bool maintenance_active);

#ifdef __cplusplus
}
#endif

#endif
