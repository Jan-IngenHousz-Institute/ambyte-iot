#include "power_policy.h"

#include <stddef.h>
#include <string.h>

/* Pure policy - see power_policy.h for the decisions and their field origin.
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

bool power_frame_same(const power_reading_t *a, const power_reading_t *b)
{
    return a->battery_mv == b->battery_mv && a->system_mv == b->system_mv &&
           a->input_mv == b->input_mv && a->charge_ma == b->charge_ma &&
           a->input_ma == b->input_ma;
}

void power_gate_init(power_gate_t *g)
{
    memset(g, 0, sizeof *g);
    g->reason = POWER_GATE_REASON_STARTING;
}

bool power_gate_step(power_gate_t *g, power_read_reason_t reason, const power_reading_t *p)
{
    const power_gate_reason_t before = g->reason;
    const bool ok = reason == POWER_READ_OK && p != NULL;

    if (!ok) {
        /* Not positive evidence of anything flashable: break the freshness
         * chain, so flashing needs two differing valid frames AFTER this. */
        g->last_ok_present = false;
        g->have_prev = false;
        g->changed_recently = false;
        g->same_streak = 0;
        if (reason == POWER_READ_NO_INPUT) {
            /* Live status: no external input. Evidence of battery power, which
             * since 2026-11-13 never stops publishing; reported only. */
            g->invalid_streak = 0;
            g->reason = POWER_GATE_REASON_ON_BATTERY;
        } else {
            if (g->invalid_streak < UINT32_MAX) g->invalid_streak++;
            g->reason = POWER_GATE_REASON_POWER_UNVERIFIED;
        }
        return g->reason != before;
    }

    g->invalid_streak = 0;
    g->reason = p->input_present ? POWER_GATE_REASON_EXTERNAL_POWER
                                 : POWER_GATE_REASON_ON_BATTERY;
    g->last_ok_present = p->input_present;

    /* Freshness for AMBIT flashing: a converting ADC jitters by LSBs between
     * reads; a frozen register file repeats bit-identically. START falling
     * cannot prove a conversion on this silicon (bench: low ~0.6 ms after the
     * write either way), so a CHANGED frame is the only freshness evidence. */
    if (g->have_prev && !power_frame_same(&g->prev, p)) {
        g->changed_recently = true;
        g->same_streak = 0;
    } else if (g->have_prev) {
        if (g->same_streak < UINT32_MAX) g->same_streak++;
        if (g->same_streak >= POWER_FLASH_FRESH_WINDOW) g->changed_recently = false;
    }
    g->prev = *p;
    g->have_prev = true;
    return g->reason != before;
}

bool power_publish_ok(const power_gate_t *g)
{
    (void)g;   /* keep sending always (2026-11-13): no reading class stops it */
    return true;
}

bool power_gate_flash_ok(const power_gate_t *g)
{
    return g != NULL && g->last_ok_present && g->changed_recently;
}

/* ── Low-battery SD park guard ─────────────────────────────────────────────
 * Same thresholds/streaks as the original app_main guard, but a read carries
 * battery evidence only when it is valid, FRESH (its frame differs from the
 * previous valid frame) and reports external input or a non-zero VBAT. */

void power_guard_init(power_guard_t *g)
{
    memset(g, 0, sizeof *g);
}

power_guard_action_t power_guard_step(power_guard_t *g, power_read_reason_t reason,
                                      const power_reading_t *p, bool maintenance_active)
{
    const bool ok = reason == POWER_READ_OK && p != NULL;
    const bool fresh = ok && g->have_prev && !power_frame_same(&g->prev, p);
    if (ok) {
        g->prev = *p;
        g->have_prev = true;
    } else {
        g->have_prev = false;
    }
    const bool evidence = fresh && (p->input_present || p->battery_mv != 0);

    if (!evidence) {
        /* Never park on no evidence; a park needs PARK_N consecutive fresh lows. */
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
        g->low_n = 0;
        /* Never yank the card out from under an OTA/AMBIT-flash/script op; the
         * battery keeps sagging, so the next polls re-trip once the op ends. */
        if (maintenance_active) return POWER_GUARD_HOLD;
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
