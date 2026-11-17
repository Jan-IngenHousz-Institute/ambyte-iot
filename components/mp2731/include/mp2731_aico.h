#ifndef AMBYTE_MP2731_AICO_H
#define AMBYTE_MP2731_AICO_H

/* Periodic re-learning for the MP2731 Automatic Input Current Optimizer: WHEN
 * the firmware rewrites REG00, as a pure state machine over the existing power
 * reads. No I2C, no clock, no logging: mp2731.c feeds every read through here
 * and performs the register access; tests/aico_policy_host.c drives this file
 * verbatim. See docs/charger-aico.md.
 *
 * Field origin (Uluru, Nov 2026): when the charger draws more than a weak
 * panel can give, the panel collapses to the VIN_MIN floor and the charger
 * starts and stops (10 stops in 25 min on one unit) until it drops the panel.
 * A current limit just under what the panel can give keeps it near 6-7 V.
 * AICO (REG02 bit 0, datasheet Rev 1.2 "Automatic Input Current Optimizer")
 * finds that limit itself: on a VIN_MIN event it lowers the input limit step
 * by step until the input leaves VIN_MIN regulation, and reports the result in
 * REG14[5:0] IIN_DPM. But it only ever LOWERS it, and "does not automatically
 * run the AICO function unless another VIN_MIN event occurs": a limit learned
 * in the weak light of dawn would cap the whole day. "Any write to IIN_LIM can
 * reset IIN_DPM to the same IIN_LIM value when the AICO function is enabled",
 * so rewriting REG00 with its own value hands AICO the full register limit
 * again; the next panel collapse re-learns from there.
 *
 * The decision: no sooner than AICO_RESET_INTERVAL_MS after the previous
 * reset, after the boot's first read, or after the operator switched AICO on,
 * the glue reads REG02. AICO_EN=0 → nothing is written and the next look is an
 * interval later. AICO_EN=1 → REG00 is read and written back unchanged under
 * one bus hold (bits 7:6 HIZ/EN_ILIM included), then verified. A failed
 * access is retried after AICO_RETRY_MS. A veto (maintenance lock, a charger
 * re-detect in progress or being judged) holds a due reset until it clears;
 * the wait is counted once. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reset period. Each reset costs one re-learn: AICO only finds the panel's
 * limit through a fresh VIN_MIN dip (the panel collapsing for the few seconds
 * the stepwise decrease takes), which is exactly the event the limit exists
 * to avoid. Without AICO the field unit above dipped every ~2.5 min; one dip
 * per hour is ~4 % of that. Irradiance on a clear day changes on the scale of
 * hours (dawn to mid-morning raises panel current several-fold over ~2-3 h),
 * so an hourly re-learn tracks the day in ~10 steps from 06:00 to 16:00,
 * while a learned limit is never more than an hour stale. A passing cloud
 * that teaches a low limit costs at most the rest of that hour. */
#define AICO_RESET_INTERVAL_MS  (60 * 60 * 1000)

/* After a failed bus access (read, write or readback): the reset has not
 * happened, so a learned limit may still cap the charger. Retry soon, but
 * not per read: a dead bus already slows every power read. */
#define AICO_RETRY_MS           (5 * 60 * 1000)

typedef enum {
    AICO_NONE = 0,
    AICO_CHECK,     /* due and allowed: glue reads REG02, resets if AICO_EN */
} aico_action_t;

typedef enum {
    AICO_RESULT_RESET = 0,  /* REG00 rewritten and read back unchanged */
    AICO_RESULT_OFF,        /* AICO_EN read 0: nothing written */
    AICO_RESULT_FAILED,     /* a bus access failed or the readback differed */
} aico_result_t;

typedef struct {
    bool     started;
    int64_t  next_ms;          /* first moment a check is allowed */
    bool     waiting_veto;     /* a due check is held by the veto (counted) */
    int8_t   enabled_seen;     /* -1 unknown, 0 off, 1 on (last REG02 look) */
    bool     has_reset;
    int64_t  last_reset_ms;    /* uptime of the last verified reset */
    uint32_t resets, failed, vetoed, checks;
} aico_t;

void aico_init(aico_t *s);

/* Feed one power read. `veto` = a reset must not happen right now. */
aico_action_t aico_step(aico_t *s, int64_t now_ms, bool veto);

/* After AICO_CHECK, or after a manual `aico reset` (counted the same way and
 * restarting the period, so resets stay at most one per interval). */
void aico_note_result(aico_t *s, int64_t now_ms, aico_result_t r);

/* AICO_CHECK was returned but the charger-mutation admission was refused
 * (maintenance began after the veto probe): nothing was read or written; held
 * like a veto (counted once per wait) until a later read is admitted. */
void aico_note_refused(aico_t *s);

/* The operator switched AICO on or off. On: a full interval before the first
 * reset, so AICO learns the panel before it is reset. */
void aico_note_enabled(aico_t *s, int64_t now_ms, bool enabled);

/* Seconds until the next allowed check (0 = due at the next read). */
uint32_t aico_next_in_s(const aico_t *s, int64_t now_ms);

/* JSON object for health.power.charger.aico from the heartbeat's register
 * scan (regs/mask as in charger_diagnostics_t: a field whose register was not
 * read is omitted, never zero-filled) and the policy counters. Returns length
 * or -1 (nothing written). AICO_RENDER_CAP bounds the worst case. */
#define AICO_RENDER_CAP 192u
int aico_render(const aico_t *s, const uint8_t *regs, uint32_t mask, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
