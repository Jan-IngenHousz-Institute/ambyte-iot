#ifndef AMBYTE_POWER_POLICY_H
#define AMBYTE_POWER_POLICY_H

/* What the firmware DOES with an MP2731 power reading, as pure state machines
 * over (verdict, reading). No I2C, no clock, no logging: device_commands.c and
 * app_main.c feed every sample through here, so tests/power_policy_host.c can
 * drive the exact production policy with a fake charger.
 *
 * Decisions of 2026-11-13, after the Uluru units ran a whole day (08:47-21:04
 * ACST) with the charger's live status saying "not charging" on every heartbeat
 * and an ADC that never converted. Units died during the day while the old
 * publish gate, misled by stale frames, held one unit's data back for 9+ h:
 *
 *  1. PUBLISH: never stop publishing while a charger is fitted. The publish
 *     gate is always open; power_gate_t only tracks WHY (reason) for telemetry.
 *     A stale-but-plausible frame cannot be told from a fresh one on this
 *     silicon (ADC_START reads low within ~0.6 ms either way), so no reading
 *     class may close it. Cost, accepted by the user: the gate no longer saves
 *     battery at night; a solar unit publishes on battery too.
 *  2. AMBIT FLASH: fail-closed. A brownout mid-flash bricks the sensor, so it
 *     needs POSITIVE, FRESH evidence of external power: the latest read valid
 *     with input present, AND the frame having changed recently (a frozen
 *     register file repeats bit-identically; a converting ADC jitters).
 *  3. SD PARK GUARD: parks only on fresh, changing evidence of a low battery on
 *     battery power; otherwise never. On this hardware the ADC does not convert
 *     without input (MP2731 datasheet p.26), so in practice it cannot fire on
 *     battery; it never acted correctly in the field before either (it compared
 *     stale frames). A parked card is remounted after
 *     POWER_POLICY_INVALID_FAILOPEN_N reads without evidence.
 * See docs/power-telemetry.md. */

#include <stdbool.h>
#include <stdint.h>

#include "sensing_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Consecutive reads without evidence before a parked SD is remounted (15 s
 * guard polls → 3 min). Also the WARN threshold for an unreadable charger. */
#define POWER_POLICY_INVALID_FAILOPEN_N 12u

/* AMBIT flash freshness: a valid frame counts as live only if it differs from
 * the previous valid frame within this many consecutive valid reads. The bench
 * (USB, charge mode) changed on nearly every read (Iin/Vsys LSB jitter); the
 * frozen Uluru frames repeated bit-identically for hours. */
#define POWER_FLASH_FRESH_WINDOW 6u

/* Why the (always-open) publish gate reports what it reports: the power state
 * as last observed. Stable telemetry strings, never rename. */
typedef enum {
    POWER_GATE_REASON_STARTING = 0,     /* no charger read yet */
    POWER_GATE_REASON_EXTERNAL_POWER,   /* valid frame, input present */
    POWER_GATE_REASON_ON_BATTERY,       /* valid frame without input, or live "no input" */
    POWER_GATE_REASON_POWER_UNVERIFIED, /* charger unreadable / stale / implausible */
    POWER_GATE_REASON_NO_CHARGER,       /* no charger wired */
} power_gate_reason_t;

const char *power_gate_reason_name(power_gate_reason_t r);

typedef struct {
    power_gate_reason_t reason;
    uint32_t invalid_streak;       /* consecutive unreadable/stale/implausible reads */
    /* AMBIT flash evidence */
    bool     last_ok_present;      /* latest read: valid AND input present */
    bool     have_prev;            /* prev_* holds the last valid frame */
    power_reading_t prev;          /* last valid frame (numeric fields compared) */
    uint32_t same_streak;          /* consecutive valid frames identical to prev */
    bool     changed_recently;     /* a valid frame differed within the window */
} power_gate_t;

void power_gate_init(power_gate_t *g);

/* Feed one read (verdict + the reading; `p` consulted only when OK). Returns
 * true when the reported reason changed (for the caller's logging). Never
 * closes anything: device_commands_publish_power_ok() is always true. */
bool power_gate_step(power_gate_t *g, power_read_reason_t reason, const power_reading_t *p);

/* The publish decision: true for every state and every reading class. It is a
 * function (not a literal at the call site) so the host tests exercise the
 * production rule itself. Never make it return false on a power reading. */
bool power_publish_ok(const power_gate_t *g);

/* AMBIT flashing: latest read valid with input present AND the frame changed
 * within POWER_FLASH_FRESH_WINDOW valid reads. Everything else refuses. */
bool power_gate_flash_ok(const power_gate_t *g);

/* True when two readings carry the same ADC frame (the five measured values). */
bool power_frame_same(const power_reading_t *a, const power_reading_t *b);

/* ── Low-battery SD park guard ───────────────────────────────────────────── */

#define POWER_GUARD_PARK_MV   3300u  /* on-battery floor: park the SD below this */
#define POWER_GUARD_PARK_N       3u  /* consecutive fresh low reads (45 s @ 15 s) */
#define POWER_GUARD_RESUME_MV 3600u  /* battery-only resume level (> park + rebound) */
#define POWER_GUARD_RESUME_N     4u  /* consecutive good reads (60 s) before un-parking */

typedef struct {
    bool     parked;
    uint32_t low_n;
    uint32_t good_n;
    uint32_t invalid_n;   /* consecutive reads carrying no fresh battery evidence */
    bool     have_prev;
    power_reading_t prev; /* last valid frame seen by the guard */
} power_guard_t;

typedef enum {
    POWER_GUARD_HOLD = 0,
    POWER_GUARD_PARK,               /* fresh low-battery evidence: park the SD */
    POWER_GUARD_UNPARK,             /* valid recovery evidence: remount */
    POWER_GUARD_UNPARK_UNVERIFIED,  /* N reads without evidence: remount */
} power_guard_action_t;

void power_guard_init(power_guard_t *g);

/* Feed one read. `maintenance_active` vetoes a PARK (never an un-park). The
 * guard has already moved to the new state when it returns PARK/UNPARK*. */
power_guard_action_t power_guard_step(power_guard_t *g, power_read_reason_t reason,
                                      const power_reading_t *p, bool maintenance_active);

#ifdef __cplusplus
}
#endif

#endif
