#include "mp2731_redetect.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mp2731_core.h"

/* Pure policy - see mp2731_redetect.h for the thresholds and their field
 * origin. Nothing here may touch I2C, the clock, or the log: the host tests
 * drive this file verbatim. */

void crd_init(crd_t *s)
{
    memset(s, 0, sizeof *s);
    s->disabled = CRD_ENABLED;
}

bool crd_eligible(const crd_obs_t *o)
{
    /* The voltages come from the ADC frame, so the frame must have been
     * accepted; the live status bytes must have completed too. */
    if (!o->status_valid || !o->adc_ok) return false;
    if (MP2731_STATUS_VIN(o->status) != 0u || MP2731_STATUS_CHARGE(o->status) != 0u) return false;
    if ((o->fault & CRD_FAULT_BLOCK_MASK) != 0u) return false;
    if ((o->status & CRD_STATUS_BLOCK_MASK) != 0u) return false;
    if (o->vsys_mv == 0u || o->vsys_mv >= CRD_VSYS_FULL_MV) return false;
    if (o->vin_mv < CRD_VIN_FLOOR_MV) return false;
    return (uint32_t)o->vin_mv >= (uint32_t)o->vsys_mv + CRD_VIN_OVER_VSYS_MV;
}

static void resolve(crd_t *s, crd_outcome_t outcome)
{
    s->pending = false;
    s->last_outcome = outcome;
    s->last_after = s->last_status_in_window;
    if (outcome == CRD_OUT_CHARGING) s->charging++;
    else if (outcome == CRD_OUT_DETECTED) s->detected++;
    else s->no_change++;
}

crd_action_t crd_step(crd_t *s, const crd_obs_t *o, bool veto)
{
    int64_t now = o->now_ms;
    /* Monotonic by contract; defensively never let time run backwards. */
    if (s->have_obs && now < s->last_obs_ms) now = s->last_obs_ms;
    const bool gap = s->have_obs && now - s->last_obs_ms > CRD_MAX_GAP_MS;
    s->have_obs = true;
    s->last_obs_ms = now;

    /* Sustained charging: only continuous live evidence counts. A bus
     * failure or a long silence restarts the run; it can never manufacture
     * ten minutes of charging. */
    const uint8_t charge = MP2731_STATUS_CHARGE(o->status);
    if (o->status_valid && (charge == 1u || charge == 2u)) {
        if (!s->charging_run || gap) {
            s->charging_run = true;
            s->charging_since_ms = now;
        } else if (now - s->charging_since_ms >= CRD_SUSTAIN_MS) {
            /* The panel holds: the next stuck episode starts from scratch. */
            s->backoff_ms = 0;
            s->has_next = false;
        }
    } else {
        s->charging_run = false;
    }

    if (s->pending) {
        if (o->status_valid) {
            s->last_status_in_window = o->status;
            if (MP2731_STATUS_VIN(o->status) != 0u) s->saw_detect = true;
            if (charge == 1u || charge == 2u) s->saw_charge = true;
        }
        if (s->saw_charge) {
            resolve(s, CRD_OUT_CHARGING);
            return CRD_RESOLVED;
        }
        if (now - s->attempt_ms >= CRD_OUTCOME_WINDOW_MS) {
            resolve(s, s->saw_detect ? CRD_OUT_DETECTED : CRD_OUT_NO_CHANGE);
            return CRD_RESOLVED;
        }
        return CRD_NONE;
    }

    if (s->disabled != CRD_ENABLED || s->attempts >= CRD_MAX_ATTEMPTS_PER_BOOT ||
        !crd_eligible(o)) {
        s->dwelling = false;
        s->veto_counted = false;
        return CRD_NONE;
    }
    if (!s->dwelling || gap) {
        s->dwelling = true;
        s->dwell_start_ms = now;
        s->dwell_changed = false;   /* a change must happen INSIDE the dwell */
        s->veto_counted = false;
        return CRD_NONE;
    }
    if (o->adc_changed) s->dwell_changed = true;
    if (now - s->dwell_start_ms < CRD_DWELL_MS || !s->dwell_changed) return CRD_NONE;
    if (s->has_next && now < s->next_allowed_ms) return CRD_NONE;
    if (veto) {
        if (!s->veto_counted) s->vetoed++;
        s->veto_counted = true;
        return CRD_NONE;
    }
    s->veto_counted = false;
    return CRD_CHECK;
}

crd_block_t crd_precheck(uint8_t reg00, uint8_t reg01, uint8_t reg04, uint8_t reg17,
                         uint16_t vin_mv)
{
    if ((reg00 & 0x80u) != 0u) return CRD_BLOCK_HIZ;
    if (((reg04 >> 4) & 0x03u) != 0x01u) return CRD_BLOCK_CHARGE_DISABLED;
    /* The pulse "clears the safety timer fault and restarts the safety timer"
     * (datasheet, Safety Timer). That timer is a battery protection: report
     * an expired one, never defeat it automatically. */
    if ((reg17 & 0x80u) != 0u) return CRD_BLOCK_SAFETY_TIMER;
    const uint32_t vin_min = 3700u + 100u * (reg01 & 0x7Fu);
    if ((uint32_t)vin_mv < vin_min + CRD_VIN_OVER_LIMIT_MV) return CRD_BLOCK_VIN_LIMIT;
    return CRD_BLOCK_NONE;
}

static void schedule_after(crd_t *s, int64_t at_ms)
{
    if (!s->has_next || at_ms > s->next_allowed_ms) s->next_allowed_ms = at_ms;
    s->has_next = true;
}

void crd_note_blocked(crd_t *s, int64_t now_ms, crd_block_t why)
{
    /* Not an attempt: no backoff growth, no attempt cap. The dwell stays
     * valid; the next check comes after the recheck interval. */
    s->blocked++;
    s->last_block = why;
    schedule_after(s, now_ms + CRD_BLOCKED_RECHECK_MS);
}

void crd_note_attempt(crd_t *s, int64_t now_ms, crd_pulse_t result, uint8_t status_before,
                      uint16_t vin_mv, uint16_t vsys_mv)
{
    s->attempts++;
    s->last_attempt_ms = now_ms;
    s->last_before = status_before;
    s->last_after = status_before;
    s->last_vin_mv = vin_mv;
    s->last_vsys_mv = vsys_mv;
    s->last_block = CRD_BLOCK_NONE;
    s->backoff_ms = s->backoff_ms == 0u ? CRD_BACKOFF_FIRST_MS
                  : (s->backoff_ms >= CRD_BACKOFF_MAX_MS / 2u ? CRD_BACKOFF_MAX_MS
                                                              : s->backoff_ms * 2u);
    /* Only the backoff is set here: next_allowed may move earlier than a
     * pending blocked recheck, so assign rather than max(). */
    s->next_allowed_ms = now_ms + s->backoff_ms;
    s->has_next = true;
    s->dwelling = false;            /* every attempt needs a fresh full dwell */
    s->veto_counted = false;
    switch (result) {
    case CRD_PULSE_OK:
        s->pending = true;
        s->attempt_ms = now_ms;
        s->saw_detect = s->saw_charge = false;
        s->last_status_in_window = status_before;
        s->last_outcome = CRD_OUT_PENDING;
        break;
    case CRD_PULSE_BUS_ERROR:
        s->failed++;
        s->last_outcome = CRD_OUT_BUS_ERROR;
        break;
    case CRD_PULSE_NOT_LATCHED:
        s->failed++;
        s->last_outcome = CRD_OUT_NOT_LATCHED;
        break;
    case CRD_PULSE_NOT_CLEARED:
        s->failed++;
        s->last_outcome = CRD_OUT_NOT_CLEARED;
        s->disabled = CRD_DISABLED_BG_EN_STUCK;
        break;
    }
}

void crd_note_manual(crd_t *s)
{
    s->manual++;
    s->dwelling = false;   /* never stack an automatic pulse on a manual one */
    s->veto_counted = false;
}

void crd_set_disabled(crd_t *s, crd_disabled_t why)
{
    s->disabled = why;
    s->dwelling = false;
    s->veto_counted = false;
}

const char *crd_outcome_name(crd_outcome_t o)
{
    switch (o) {
    case CRD_OUT_NONE:        return "none";
    case CRD_OUT_PENDING:     return "pending";
    case CRD_OUT_CHARGING:    return "charging";
    case CRD_OUT_DETECTED:    return "detected";
    case CRD_OUT_NO_CHANGE:   return "no_change";
    case CRD_OUT_BUS_ERROR:   return "bus_error";
    case CRD_OUT_NOT_LATCHED: return "not_latched";
    case CRD_OUT_NOT_CLEARED: return "not_cleared";
    }
    return "none";
}

const char *crd_block_name(crd_block_t b)
{
    switch (b) {
    case CRD_BLOCK_NONE:            return "none";
    case CRD_BLOCK_HIZ:             return "hiz";
    case CRD_BLOCK_CHARGE_DISABLED: return "charge_disabled";
    case CRD_BLOCK_SAFETY_TIMER:    return "safety_timer";
    case CRD_BLOCK_VIN_LIMIT:       return "vin_limit";
    case CRD_BLOCK_BUS:             return "bus_error";
    }
    return "none";
}

const char *crd_disabled_name(crd_disabled_t d)
{
    switch (d) {
    case CRD_ENABLED:              return "none";
    case CRD_DISABLED_OPERATOR:    return "operator";
    case CRD_DISABLED_RESET:       return "reset_during_attempt";
    case CRD_DISABLED_BG_EN_STUCK: return "bg_en_stuck";
    }
    return "none";
}

const char *crd_state_name(const crd_t *s, int64_t now_ms)
{
    if (s->pending) return "pending";
    if (s->disabled != CRD_ENABLED) return "disabled";
    if (s->attempts >= CRD_MAX_ATTEMPTS_PER_BOOT) return "capped";
    if (s->has_next && now_ms < s->next_allowed_ms) return "backoff";
    if (s->dwelling) return "dwell";
    return "idle";
}

static void append(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    if (*used >= cap) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + *used, cap - *used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap - *used) { *used = cap; return; }
    *used += (size_t)n;
}

int crd_render(const crd_t *s, int64_t now_ms, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return -1;
    size_t n = 0;
    const int64_t dwell = s->dwelling && now_ms > s->dwell_start_ms ? now_ms - s->dwell_start_ms : 0;
    const int64_t next = s->has_next && s->next_allowed_ms > now_ms ? s->next_allowed_ms - now_ms : 0;
    append(out, cap, &n, "{\"auto\":%s,\"state\":\"%s\",\"disabled_reason\":",
           s->disabled == CRD_ENABLED ? "true" : "false", crd_state_name(s, now_ms));
    if (s->disabled == CRD_ENABLED) append(out, cap, &n, "null");
    else append(out, cap, &n, "\"%s\"", crd_disabled_name(s->disabled));
    append(out, cap, &n, ",\"attempts\":%u,\"charging\":%u,\"detected\":%u,\"no_change\":%u,"
           "\"failed\":%u,\"blocked\":%u,\"vetoed\":%u,\"manual\":%u,\"dwell_s\":%lld,"
           "\"backoff_s\":%u,\"next_in_s\":%lld,\"last_block\":",
           (unsigned)s->attempts, (unsigned)s->charging, (unsigned)s->detected,
           (unsigned)s->no_change, (unsigned)s->failed, (unsigned)s->blocked,
           (unsigned)s->vetoed, (unsigned)s->manual, (long long)(dwell / 1000),
           (unsigned)(s->backoff_ms / 1000u), (long long)(next / 1000 + (next % 1000 != 0)));
    if (s->last_block == CRD_BLOCK_NONE) append(out, cap, &n, "null");
    else append(out, cap, &n, "\"%s\"", crd_block_name(s->last_block));
    append(out, cap, &n, ",\"last\":");
    if (s->attempts == 0u) {
        append(out, cap, &n, "null}");
    } else {
        append(out, cap, &n, "{\"uptime_ms\":%lld,\"outcome\":\"%s\",\"reg0c_before\":%u,"
               "\"reg0c_after\":%u,\"vin_mv\":%u,\"vsys_mv\":%u}}",
               (long long)s->last_attempt_ms, crd_outcome_name(s->last_outcome),
               s->last_before, s->last_after, s->last_vin_mv, s->last_vsys_mv);
    }
    if (n >= cap) { out[0] = '\0'; return -1; }
    return (int)n;
}
