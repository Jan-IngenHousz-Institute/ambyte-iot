#include "power_policy.h"

#include <stddef.h>
#include <string.h>

/* Pure policy — see power_policy.h for the rule and its field-incident origin.
 * Nothing here may touch I2C, the clock, or the log: the host tests drive this
 * file verbatim. */

const char *power_gate_reason_name(power_gate_reason_t r)
{
    /* Stable telemetry spelling (health.connectivity.publish_gate_reason). */
    switch (r) {
    case POWER_GATE_REASON_STARTING:         return "starting";
    case POWER_GATE_REASON_EXTERNAL_POWER:   return "external_power";
    case POWER_GATE_REASON_ON_BATTERY:       return "on_battery";
    case POWER_GATE_REASON_POWER_UNVERIFIED: return "power_unverified";
    case POWER_GATE_REASON_NO_CHARGER:       return "no_charger";
    }
    return "starting";
}

void power_gate_init(power_gate_t *g)
{
    memset(g, 0, sizeof *g);
    g->reason = POWER_GATE_REASON_STARTING;   /* closed at boot, as before */
}

power_gate_step_t power_gate_step(power_gate_t *g, int64_t now_ms,
                                  power_read_reason_t reason, bool present)
{
    if (reason != POWER_READ_OK) {
        g->last_valid = false;
        g->verify_ms = 0;
        if (g->invalid_streak < UINT32_MAX) g->invalid_streak++;

        /* An invalid sample is no evidence that power went away, so it must not
         * let a pending CLOSE mature: cancel it. A close then needs a fresh
         * OFF dwell of VALID "absent" readings. (Before this, a read error
         * merely held state, so error samples could silently age a close
         * candidate and the next valid sample flipped the gate at once.)
         * A pending OPEN is left running — invalid samples never hold the
         * gate closed. */
        if (g->open) g->cand = true;

        if (!g->open && g->invalid_streak >= POWER_POLICY_INVALID_FAILOPEN_N) {
            g->open = true;
            g->cand = true;
            g->reason = POWER_GATE_REASON_POWER_UNVERIFIED;
            return POWER_GATE_STEP_FAILOPEN;
        }
        return POWER_GATE_STEP_NONE;
    }

    /* Valid evidence: exactly the Phase 1 debounce, starting from wherever the
     * gate is (including a fail-open — a valid "on battery" reading closes it
     * after the normal OFF dwell). */
    g->last_valid = true;
    g->invalid_streak = 0;

    if (present == g->open) {
        g->cand = present;                 /* steady — clear any pending change */
        if (!present) {
            g->reason = POWER_GATE_REASON_ON_BATTERY;
        } else if (g->reason == POWER_GATE_REASON_POWER_UNVERIFIED) {
            /* Open on the fail-open fallback and the charger reads again: the
             * gate is already open, but the reason (and with it AMBIT flashing)
             * upgrades to "external_power" only after the same ON dwell a
             * closed gate would need — one good frame after a frozen spell is
             * not yet evidence. */
            if (g->verify_ms == 0) g->verify_ms = now_ms != 0 ? now_ms : 1;
            if ((now_ms - g->verify_ms) >= POWER_GATE_ON_DWELL_MS) {
                g->reason = POWER_GATE_REASON_EXTERNAL_POWER;
                g->verify_ms = 0;
            }
        } else {
            g->reason = POWER_GATE_REASON_EXTERNAL_POWER;
        }
        return POWER_GATE_STEP_NONE;
    }
    g->verify_ms = 0;
    if (present != g->cand) {
        g->cand = present;                 /* new candidate — start the dwell timer */
        g->cand_ms = now_ms;
        return POWER_GATE_STEP_NONE;
    }
    const int64_t dwell = present ? POWER_GATE_ON_DWELL_MS : POWER_GATE_OFF_DWELL_MS;
    if ((now_ms - g->cand_ms) < dwell) return POWER_GATE_STEP_NONE;

    g->open = present;                     /* debounce satisfied — flip the gate */
    g->reason = present ? POWER_GATE_REASON_EXTERNAL_POWER : POWER_GATE_REASON_ON_BATTERY;
    return present ? POWER_GATE_STEP_OPENED : POWER_GATE_STEP_CLOSED;
}

bool power_gate_flash_ok(const power_gate_t *g)
{
    return g != NULL && g->open && g->last_valid &&
           g->reason == POWER_GATE_REASON_EXTERNAL_POWER;
}

/* ── Low-battery SD park guard ─────────────────────────────────────────────
 * Same thresholds/streaks as the original app_main guard. A sample carries
 * battery evidence only when it is valid AND either reports external input or a
 * non-zero battery voltage: the MP2731 frame with battery_mv == 0 and no input
 * says nothing about how low the pack is (the original guard skipped it too). */

void power_guard_init(power_guard_t *g)
{
    memset(g, 0, sizeof *g);
}

power_guard_action_t power_guard_step(power_guard_t *g, power_read_reason_t reason,
                                      const power_reading_t *p, bool maintenance_active)
{
    const bool evidence = reason == POWER_READ_OK && p != NULL &&
                          (p->input_present || p->battery_mv != 0);
    if (!evidence) {
        /* Never park on no evidence; a park needs PARK_N consecutive VALID lows. */
        g->low_n = 0;
        if (g->invalid_n < UINT32_MAX) g->invalid_n++;
        if (g->parked && g->invalid_n >= POWER_POLICY_INVALID_FAILOPEN_N) {
            /* Never STAY parked on no evidence either: a frozen charger would
             * otherwise keep the card (archive, overflow of unsent records,
             * sd_logger) offline forever. */
            g->parked = false;
            g->good_n = 0;
            g->invalid_n = 0;
            return POWER_GUARD_UNPARK_UNVERIFIED;
        }
        return POWER_GUARD_HOLD;
    }
    g->invalid_n = 0;

    if (!g->parked) {
        g->low_n = (!p->input_present && p->battery_mv < POWER_GUARD_PARK_MV) ? g->low_n + 1 : 0;
        if (g->low_n < POWER_GUARD_PARK_N) return POWER_GUARD_HOLD;
        /* Never yank the card out from under an OTA/AMBIT-flash/script op; the
         * battery keeps sagging, so the next poll re-trips once the op ends. */
        if (maintenance_active) {
            g->low_n = 0;
            return POWER_GUARD_HOLD;
        }
        g->low_n = 0;
        g->good_n = 0;
        g->parked = true;
        return POWER_GUARD_PARK;
    }

    g->good_n = (p->input_present || p->battery_mv >= POWER_GUARD_RESUME_MV) ? g->good_n + 1 : 0;
    if (g->good_n < POWER_GUARD_RESUME_N) return POWER_GUARD_HOLD;
    g->good_n = 0;
    g->low_n = 0;
    g->parked = false;
    return POWER_GUARD_UNPARK;
}
