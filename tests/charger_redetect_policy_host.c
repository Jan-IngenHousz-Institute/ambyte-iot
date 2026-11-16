/* Host harness: the production automatic re-detection policy
 * (components/mp2731/mp2731_redetect.c) driven verbatim by synthetic reads.
 * tests/test_charger_redetect.py runs every scenario (exit 0 = pass).
 *
 * The "field" scenario replays the 17 Nov 2026 Uluru capture of
 * 14:63:93:39:36:F8 from ~07:22: REG0C 0x00, REG0D 0x80, VIN 7.4-7.6 V,
 * VSYS 3.5 V, one read every 5 s. Values are from the heartbeat register
 * scans; the 5 s cadence and the exact LSB jitter are modeled. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp2731_redetect.h"

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

#define S 1000LL
#define MIN (60 * S)

static int64_t now_ms;
static unsigned jitter;

/* The stuck field state, with the ADC jittering by an LSB (fresh frames). */
static crd_obs_t stuck(void)
{
    jitter++;
    return (crd_obs_t){.now_ms = now_ms, .status_valid = true, .adc_ok = true,
                       .adc_changed = true, .status = 0x00, .fault = 0x80,
                       .vin_mv = (uint16_t)(7440 + 60 * (jitter % 3)), .vsys_mv = 3500};
}

static crd_obs_t with_status(uint8_t status)
{
    crd_obs_t o = stuck();
    o.status = status;
    return o;
}

/* Feed `o` every 5 s for `ms`; return the first CHECK time or -1. */
static int64_t run_until_check(crd_t *s, crd_obs_t (*make)(void), int64_t ms, bool veto)
{
    const int64_t end = now_ms + ms;
    for (; now_ms <= end; now_ms += 5 * S) {
        crd_obs_t o = make();
        if (crd_step(s, &o, veto) == CRD_CHECK) return now_ms;
    }
    return -1;
}

/* Feed a fixed observation every 5 s, return true if any CHECK. */
static bool any_check(crd_t *s, crd_obs_t o, int64_t ms)
{
    const int64_t end = now_ms + ms;
    for (; now_ms <= end; now_ms += 5 * S) {
        o.now_ms = now_ms;
        o.vin_mv = (uint16_t)(o.vin_mv ^ 0); /* constant on purpose */
        if (crd_step(s, &o, false) == CRD_CHECK) return true;
    }
    return false;
}

static void reset(crd_t *s)
{
    crd_init(s);
    now_ms = 100 * S;
    jitter = 0;
}

/* Attempt with a pulse that "did nothing": the window closes with no change. */
static void attempt_no_change(crd_t *s)
{
    crd_note_attempt(s, now_ms, CRD_PULSE_OK, 0x00, 7500, 3500);
    CHECK(s->pending, "attempt opens the window");
    crd_action_t a = CRD_NONE;
    while (a != CRD_RESOLVED) {
        now_ms += 5 * S;
        crd_obs_t o = stuck();
        a = crd_step(s, &o, false);
        CHECK(a != CRD_CHECK, "never re-checks while pending");
    }
    CHECK(s->last_outcome == CRD_OUT_NO_CHANGE, "outcome %s", crd_outcome_name(s->last_outcome));
}

static void scenario_field(void)
{
    crd_t s; reset(&s);
    const int64_t start = now_ms;
    const int64_t at = run_until_check(&s, stuck, 30 * MIN, false);
    CHECK(at >= 0, "the 17 Nov stuck state must trigger");
    CHECK(at - start >= CRD_DWELL_MS, "not before the dwell (%lld ms)", (long long)(at - start));
    CHECK(at - start <= CRD_DWELL_MS + 10 * S, "promptly after the dwell (%lld ms)", (long long)(at - start));
    CHECK(crd_precheck(0x48, 0x06, 0x5B, 0x00, 7500) == CRD_BLOCK_NONE,
          "17 Nov registers REG00 0x48 / REG04 0x5B / REG17 0x00 pass the precheck");
    CHECK(crd_precheck(0x48, 0x0D, 0x5B, 0x00, 7500) == CRD_BLOCK_NONE,
          "5.0 V VIN limit (test on 17 Nov) still passes at 7.5 V");
    crd_note_attempt(&s, now_ms, CRD_PULSE_OK, 0x00, 7500, 3500);
    /* The chip re-detects and charges 2 reads later. */
    now_ms += 5 * S; crd_obs_t o = with_status(0x20);
    CHECK(crd_step(&s, &o, false) == CRD_NONE, "detected, not yet charging");
    now_ms += 5 * S; o = with_status(0x30);
    CHECK(crd_step(&s, &o, false) == CRD_RESOLVED, "charging resolves at once");
    CHECK(s.last_outcome == CRD_OUT_CHARGING && s.charging == 1 && s.attempts == 1, "counted");
    CHECK(s.last_before == 0x00 && s.last_after == 0x30, "before/after REG0C");
    puts("OK field");
}

static void scenario_never(void)
{
    struct { const char *why; crd_obs_t o; } cases[] = {
        {"trickle", with_status(0x08)}, {"cc charging", with_status(0x30)},
        {"done", with_status(0x18)}, {"qualified idle", with_status(0x20)},
        {"ntc float", with_status(0x04)}, {"thermal regulation", with_status(0x02)},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        crd_t s; reset(&s);
        CHECK(!any_check(&s, cases[i].o, 2 * 60 * MIN), "%s must never trigger", cases[i].why);
    }
    const uint8_t faults[] = {0x01, 0x02, 0x03, 0x05, 0x06, 0x08, 0x10, 0x20, 0x40, 0xA0};
    for (unsigned i = 0; i < sizeof faults; i++) {
        crd_t s; reset(&s); crd_obs_t o = stuck(); o.fault = faults[i];
        CHECK(!any_check(&s, o, 60 * MIN), "fault 0x%02X must block", faults[i]);
    }
    struct { const char *why; uint16_t vin, vsys; } volts[] = {
        {"5 V USB source", 5160, 3500}, {"USB at 5.25 V spec max", 5280, 3500},
        {"twilight 5.46 V", 5460, 3500},
        {"full battery", 7500, 4100}, {"full battery 4.2 V", 7500, 4200}, {"no VSYS", 7500, 0},
    };
    for (unsigned i = 0; i < sizeof volts / sizeof *volts; i++) {
        crd_t s; reset(&s); crd_obs_t o = stuck(); o.vin_mv = volts[i].vin; o.vsys_mv = volts[i].vsys;
        o.adc_changed = true;
        CHECK(!any_check(&s, o, 60 * MIN), "%s must never trigger", volts[i].why);
    }
    /* VIN >= VSYS + 1 V is implied today by the 5.5 V floor and the 4.1 V
     * full gate (5.5 - 4.1 = 1.4 V); it is kept so that a lower floor can
     * never drop the headroom rule. Pin it directly. */
    { crd_obs_t o = stuck(); o.vin_mv = 5500; o.vsys_mv = 4099; CHECK(crd_eligible(&o), "1.4 V over");
      o.vsys_mv = 4600; CHECK(!crd_eligible(&o), "full gate"); }
    { crd_t s; reset(&s); crd_obs_t o = stuck(); o.adc_ok = false;
      CHECK(!any_check(&s, o, 60 * MIN), "rejected ADC frame (no trusted VIN) never triggers"); }
    { crd_t s; reset(&s); crd_obs_t o = stuck(); o.status_valid = false;
      CHECK(!any_check(&s, o, 60 * MIN), "bus failure never triggers"); }
    { crd_t s; reset(&s); crd_obs_t o = stuck(); o.adc_changed = false;
      CHECK(!any_check(&s, o, 3 * 60 * MIN), "a frozen register file never triggers"); }
    { crd_t s; reset(&s); crd_obs_t o = stuck(); o.vin_mv = 5500; o.vsys_mv = 3500;
      CHECK(any_check(&s, o, 10 * MIN), "exactly at the floor triggers"); }
    puts("OK never");
}

static void scenario_continuity(void)
{
    crd_t s; reset(&s);
    /* 2.5 min stuck, then one charging read: the dwell restarts. */
    CHECK(run_until_check(&s, stuck, 150 * S, false) < 0, "short dwell");
    crd_obs_t o = with_status(0x30); now_ms += 5 * S; crd_step(&s, &o, false);
    const int64_t restart = now_ms + 5 * S; now_ms = restart;
    const int64_t at = run_until_check(&s, stuck, 30 * MIN, false);
    CHECK(at - restart >= CRD_DWELL_MS, "dwell restarted after charging (%lld)", (long long)(at - restart));

    /* A silence longer than CRD_MAX_GAP_MS restarts it too. */
    reset(&s);
    CHECK(run_until_check(&s, stuck, 170 * S, false) < 0, "short dwell");
    now_ms += CRD_MAX_GAP_MS + S;
    const int64_t after_gap = now_ms;
    const int64_t at2 = run_until_check(&s, stuck, 30 * MIN, false);
    CHECK(at2 - after_gap >= CRD_DWELL_MS, "gap restarts the dwell (%lld)", (long long)(at2 - after_gap));

    /* Time running backwards never fires early. */
    reset(&s);
    CHECK(run_until_check(&s, stuck, 100 * S, false) < 0, "short dwell");
    now_ms -= 50 * S;
    CHECK(run_until_check(&s, stuck, 30 * S, false) < 0, "backwards clock cannot shorten the dwell");
    puts("OK continuity");
}

static void scenario_backoff(void)
{
    crd_t s; reset(&s);
    int64_t prev = run_until_check(&s, stuck, 30 * MIN, false);
    CHECK(prev > 0, "first");
    attempt_no_change(&s);
    const int64_t expect[] = {10 * MIN, 20 * MIN, 40 * MIN, 60 * MIN, 60 * MIN, 60 * MIN};
    int64_t attempt_at = prev;
    for (unsigned i = 0; i < sizeof expect / sizeof *expect; i++) {
        const int64_t at = run_until_check(&s, stuck, 3 * 60 * MIN, false);
        CHECK(at > 0, "attempt %u", i + 2);
        const int64_t gap = at - attempt_at;
        CHECK(gap >= expect[i] && gap <= expect[i] + 10 * S,
              "attempt %u after %lld s, expected %lld s", i + 2, (long long)(gap / S),
              (long long)(expect[i] / S));
        attempt_at = at;
        attempt_no_change(&s);
    }
    CHECK(s.attempts == 7 && s.no_change == 7 && s.charging == 0, "counters");

    /* Per-boot cap. */
    while (s.attempts < CRD_MAX_ATTEMPTS_PER_BOOT) {
        CHECK(run_until_check(&s, stuck, 3 * 60 * MIN, false) > 0, "attempt %u", s.attempts + 1);
        attempt_no_change(&s);
    }
    CHECK(run_until_check(&s, stuck, 10 * 60 * MIN, false) < 0, "capped");
    CHECK(!strcmp(crd_state_name(&s, now_ms), "capped"), "state %s", crd_state_name(&s, now_ms));
    puts("OK backoff");
}

/* A panel that charges a few seconds and collapses (17 Nov 06:56-07:21):
 * outcome "charging", but the backoff keeps growing. */
static void scenario_collapse(void)
{
    crd_t s; reset(&s);
    int64_t attempt_at = run_until_check(&s, stuck, 30 * MIN, false);
    for (unsigned i = 0; i < 4; i++) {
        crd_note_attempt(&s, now_ms, CRD_PULSE_OK, 0x00, 7500, 3500);
        now_ms += 5 * S; crd_obs_t o = with_status(0x30);
        CHECK(crd_step(&s, &o, false) == CRD_RESOLVED && s.last_outcome == CRD_OUT_CHARGING, "burst");
        for (int k = 0; k < 3; k++) { now_ms += 5 * S; o = with_status(0x30); crd_step(&s, &o, false); }
        const int64_t at = run_until_check(&s, stuck, 3 * 60 * MIN, false);
        const int64_t want = i == 0 ? 10 * MIN : i == 1 ? 20 * MIN : i == 2 ? 40 * MIN : 60 * MIN;
        CHECK(at - attempt_at >= want, "collapse %u: retry after %lld s, want >= %lld s", i,
              (long long)((at - attempt_at) / S), (long long)(want / S));
        attempt_at = at;
    }
    CHECK(s.charging == 4 && s.attempts == 4, "every burst counted as charging");
    puts("OK collapse");
}

/* Ten minutes of continuous charging prove the panel holds: backoff resets. */
static void scenario_sustain(void)
{
    crd_t s; reset(&s);
    for (int i = 0; i < 3; i++) {
        CHECK(run_until_check(&s, stuck, 3 * 60 * MIN, false) > 0, "attempt");
        attempt_no_change(&s);
    }
    CHECK(s.backoff_ms == 40u * 60u * 1000u, "backoff grown");
    for (int64_t end = now_ms + CRD_SUSTAIN_MS + 5 * S; now_ms <= end; now_ms += 5 * S) {
        crd_obs_t o = with_status(0x30); crd_step(&s, &o, false);
    }
    CHECK(s.backoff_ms == 0 && !s.has_next, "sustained charging resets the backoff");
    const int64_t start = now_ms;
    const int64_t at = run_until_check(&s, stuck, 60 * MIN, false);
    CHECK(at - start <= CRD_DWELL_MS + 10 * S, "next stuck episode: retry after the dwell only");

    /* A bus failure breaks the run: 9 min + error + 9 min is NOT sustained. */
    reset(&s);
    CHECK(run_until_check(&s, stuck, 3 * 60 * MIN, false) > 0, "attempt");
    attempt_no_change(&s);
    for (int half = 0; half < 2; half++) {
        for (int64_t end = now_ms + 9 * MIN; now_ms <= end; now_ms += 5 * S) {
            crd_obs_t o = with_status(0x30); crd_step(&s, &o, false);
        }
        crd_obs_t bad = stuck(); bad.status_valid = false; bad.adc_ok = false;
        now_ms += 5 * S; crd_step(&s, &bad, false);
    }
    CHECK(s.backoff_ms == CRD_BACKOFF_FIRST_MS, "interrupted charging keeps the backoff");
    puts("OK sustain");
}

static void scenario_outcomes(void)
{
    crd_t s; reset(&s);
    run_until_check(&s, stuck, 30 * MIN, false);
    crd_note_attempt(&s, now_ms, CRD_PULSE_OK, 0x00, 7500, 3500);
    crd_action_t a = CRD_NONE; int64_t t0 = now_ms;
    while (a != CRD_RESOLVED) { now_ms += 5 * S; crd_obs_t o = with_status(0x20); a = crd_step(&s, &o, false); }
    CHECK(s.last_outcome == CRD_OUT_DETECTED && s.detected == 1, "detected only");
    CHECK(now_ms - t0 >= CRD_OUTCOME_WINDOW_MS, "window respected");

    /* A bus-invalid read inside the window does not count as either. */
    reset(&s);
    run_until_check(&s, stuck, 30 * MIN, false);
    crd_note_attempt(&s, now_ms, CRD_PULSE_OK, 0x00, 7500, 3500);
    a = CRD_NONE;
    while (a != CRD_RESOLVED) {
        now_ms += 5 * S; crd_obs_t o = with_status(0x30); o.status_valid = false;
        a = crd_step(&s, &o, false);
    }
    CHECK(s.last_outcome == CRD_OUT_NO_CHANGE, "invalid reads prove nothing");

    /* Pulse failures: counted as attempts, never pending, backoff applies. */
    const crd_pulse_t fails[] = {CRD_PULSE_BUS_ERROR, CRD_PULSE_NOT_LATCHED};
    const crd_outcome_t outs[] = {CRD_OUT_BUS_ERROR, CRD_OUT_NOT_LATCHED};
    for (unsigned i = 0; i < 2; i++) {
        reset(&s);
        run_until_check(&s, stuck, 30 * MIN, false);
        crd_note_attempt(&s, now_ms, fails[i], 0x00, 7500, 3500);
        CHECK(!s.pending && s.failed == 1 && s.last_outcome == outs[i], "failure %u", i);
        CHECK(s.has_next && s.next_allowed_ms == now_ms + CRD_BACKOFF_FIRST_MS, "backoff after failure");
        CHECK(s.disabled == CRD_ENABLED, "a bus error does not disable");
    }
    reset(&s);
    run_until_check(&s, stuck, 30 * MIN, false);
    crd_note_attempt(&s, now_ms, CRD_PULSE_NOT_CLEARED, 0x00, 7500, 3500);
    CHECK(s.disabled == CRD_DISABLED_BG_EN_STUCK, "stuck BG_EN disables automatic re-detection");
    CHECK(run_until_check(&s, stuck, 5 * 60 * MIN, false) < 0, "and nothing fires afterwards");
    puts("OK outcomes");
}

static void scenario_precheck(void)
{
    CHECK(crd_precheck(0xC8, 6, 0x5B, 0, 7500) == CRD_BLOCK_HIZ, "hiz");
    CHECK(crd_precheck(0x48, 6, 0x4B, 0, 7500) == CRD_BLOCK_CHARGE_DISABLED, "charge disabled");
    CHECK(crd_precheck(0x48, 6, 0x7B, 0, 7500) == CRD_BLOCK_CHARGE_DISABLED, "otg");
    CHECK(crd_precheck(0x48, 6, 0x5B, 0x80, 7500) == CRD_BLOCK_SAFETY_TIMER, "safety timer");
    CHECK(crd_precheck(0x48, 6, 0x5B, 0x00, 4799) == CRD_BLOCK_VIN_LIMIT, "4.3 V limit + 0.5 V");
    CHECK(crd_precheck(0x48, 6, 0x5B, 0x00, 4800) == CRD_BLOCK_NONE, "at the margin");
    CHECK(crd_precheck(0x48, 33, 0x5B, 0x00, 7400) == CRD_BLOCK_VIN_LIMIT, "7.0 V limit");
    CHECK(crd_precheck(0x48, 6, 0x5B, 0x38, 7500) == CRD_BLOCK_NONE, "PN bits ignored");

    crd_t s; reset(&s);
    run_until_check(&s, stuck, 30 * MIN, false);
    crd_note_blocked(&s, now_ms, CRD_BLOCK_SAFETY_TIMER);
    const int64_t blocked_at = now_ms;
    CHECK(s.attempts == 0 && s.blocked == 1, "a block is not an attempt");
    const int64_t at = run_until_check(&s, stuck, 60 * MIN, false);
    CHECK(at - blocked_at >= CRD_BLOCKED_RECHECK_MS && at - blocked_at <= CRD_BLOCKED_RECHECK_MS + 10 * S,
          "recheck after %lld s", (long long)((at - blocked_at) / S));
    char json[CRD_RENDER_CAP];
    CHECK(crd_render(&s, now_ms, json, sizeof json) > 0 && strstr(json, "\"last_block\":\"safety_timer\""),
          "%s", json);
    puts("OK precheck");
}

static void scenario_veto_and_switch(void)
{
    crd_t s; reset(&s);
    CHECK(run_until_check(&s, stuck, 30 * MIN, true) < 0, "never under the maintenance lock");
    CHECK(s.vetoed == 1, "one veto per wait, not per read (%u)", s.vetoed);
    const int64_t clear = now_ms;
    const int64_t at = run_until_check(&s, stuck, 30 * MIN, false);
    CHECK(at >= 0 && at - clear <= 5 * S, "fires as soon as the lock is released");

    reset(&s);
    crd_set_disabled(&s, CRD_DISABLED_OPERATOR);
    CHECK(run_until_check(&s, stuck, 60 * MIN, false) < 0, "operator off");
    char json[CRD_RENDER_CAP];
    CHECK(crd_render(&s, now_ms, json, sizeof json) > 0 && strstr(json, "\"auto\":false") &&
          strstr(json, "\"disabled_reason\":\"operator\""), "%s", json);
    crd_set_disabled(&s, CRD_ENABLED);
    const int64_t on = now_ms;
    const int64_t at2 = run_until_check(&s, stuck, 60 * MIN, false);
    CHECK(at2 - on >= CRD_DWELL_MS, "re-enabled: a fresh dwell");

    reset(&s);
    run_until_check(&s, stuck, 120 * S, false);
    crd_note_manual(&s);
    const int64_t manual = now_ms;
    const int64_t at3 = run_until_check(&s, stuck, 60 * MIN, false);
    CHECK(at3 - manual >= CRD_DWELL_MS && s.manual == 1, "a manual pulse restarts the dwell");
    puts("OK veto");
}

static void scenario_render(void)
{
    crd_t s; reset(&s);
    char json[CRD_RENDER_CAP];
    for (int i = 0; i < 2; i++) {
        int n = crd_render(&s, now_ms, json, sizeof json);
        CHECK(n > 0 && (size_t)n == strlen(json), "render");
        printf("%s=%s\n", i == 0 ? "RENDER_FRESH" : "RENDER_PENDING", json);
        run_until_check(&s, stuck, 30 * MIN, false);
        crd_note_attempt(&s, now_ms, CRD_PULSE_OK, 0x00, 7500, 3500);
    }
    /* Worst case: every counter and timestamp at its widest. */
    memset(&s, 0, sizeof s);
    s.disabled = CRD_DISABLED_RESET;
    s.attempts = s.charging = s.detected = s.no_change = s.failed = s.blocked = s.vetoed = s.manual = UINT32_MAX;
    s.backoff_ms = UINT32_MAX; s.has_next = true; s.next_allowed_ms = INT64_MAX;
    s.dwelling = true; s.dwell_start_ms = INT64_MIN + 1; s.last_attempt_ms = INT64_MIN;
    s.last_outcome = CRD_OUT_NOT_CLEARED; s.last_block = CRD_BLOCK_CHARGE_DISABLED;
    s.last_vin_mv = s.last_vsys_mv = UINT16_MAX; s.last_before = s.last_after = 0xFF;
    const int n = crd_render(&s, 0, json, sizeof json);
    CHECK(n > 0, "worst case fits CRD_RENDER_CAP");
    printf("RENDER_WORST=%s\n", json);
    CHECK(crd_render(&s, 0, json, 40) == -1 && json[0] == '\0', "truncation is refused, never partial");
    puts("OK render");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const char *n = argv[1];
    if (!strcmp(n, "field")) scenario_field();
    else if (!strcmp(n, "never")) scenario_never();
    else if (!strcmp(n, "continuity")) scenario_continuity();
    else if (!strcmp(n, "backoff")) scenario_backoff();
    else if (!strcmp(n, "collapse")) scenario_collapse();
    else if (!strcmp(n, "sustain")) scenario_sustain();
    else if (!strcmp(n, "outcomes")) scenario_outcomes();
    else if (!strcmp(n, "precheck")) scenario_precheck();
    else if (!strcmp(n, "veto")) scenario_veto_and_switch();
    else if (!strcmp(n, "render")) scenario_render();
    else return 2;
    return 0;
}
