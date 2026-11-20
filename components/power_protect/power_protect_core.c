#include "power_protect_core.h"

#include <stdio.h>
#include <string.h>

/* Pure state machine — contract and field origin in power_protect_core.h.
 * Nothing here may touch NVS, the clock, FreeRTOS or the log: the host tests
 * (tests/power_protect_host.c) compile this file verbatim. */

void pp_core_init(pp_core_t *c)
{
    memset(c, 0, sizeof *c);
    c->p.state = PP_STATE_NORMAL;
    c->p.reason = PP_REASON_NONE;
}

void pp_load_unreadable(pp_core_t *c, pp_clock_t clk)
{
    /* Review of PR #103: defaulting to normal let a unit in a (forced) hold
     * whose blob was corrupted resume full load on its next non-brownout
     * reboot. We cannot know what was stored, so assume the protected side
     * that still lets the unit measure and recover by itself: no_flash, not
     * forced (accumulated evidence, the probe or clear ends it). */
    pp_core_init(c);
    c->p.state = PP_STATE_NO_FLASH;
    c->p.reason = PP_REASON_STATE_UNREADABLE;
    c->p.since_utc = clk.valid ? clk.utc : 0;
    /* The failed-probe count is lost with the blob: assume the longest wait
     * (level cap), not a fresh 6 h, and none on probation. */
    c->p.probe_fails = PP_PROBE_LEVEL_CAP;
    c->persist_fault = true;
}

/* ── names ─────────────────────────────────────────────────────────────── */

const char *pp_state_name(pp_state_t s)
{
    switch (s) {
    case PP_STATE_NORMAL:   return "normal";
    case PP_STATE_NO_FLASH: return "no_flash";
    case PP_STATE_HOLD:     return "hold";
    }
    return "normal";
}

const char *pp_reason_name(pp_reason_t r)
{
    switch (r) {
    case PP_REASON_NONE:            return "none";
    case PP_REASON_BROWNOUT:        return "brownout";
    case PP_REASON_REPEAT_BROWNOUT: return "repeat_brownout";
    case PP_REASON_LOW_VOLTAGE:     return "low_voltage";
    case PP_REASON_FORCED:          return "forced";
    case PP_REASON_POWER_RECOVERED: return "power_recovered";
    case PP_REASON_CLEARED:         return "cleared";
    case PP_REASON_STATE_UNREADABLE: return "state_unreadable";
    case PP_REASON_BROWNOUT_UNORDERED: return "brownout_unordered";
    case PP_REASON_PROBE:           return "probe";
    case PP_REASON_PROBE_FAILED:    return "probe_failed";
    case PP_REASON_COUNT:           break;
    }
    return "none";
}

const char *pp_probe_name(pp_probe_t p)
{
    switch (p) {
    case PP_PROBE_OFF:        return "off";
    case PP_PROBE_WAITING:    return "waiting";
    case PP_PROBE_PROBATION:  return "probation";
    case PP_PROBE_EXHAUSTED:  return "exhausted";
    case PP_PROBE_NO_MONITOR: return "no_monitor";
    }
    return "off";
}

bool pp_force_state_parse(const char *s, pp_state_t *out)
{
    if (s == NULL) return false;
    if (strcmp(s, "no_flash") == 0) { *out = PP_STATE_NO_FLASH; return true; }
    if (strcmp(s, "hold") == 0)     { *out = PP_STATE_HOLD;     return true; }
    return false;
}

/* ── blob codec ────────────────────────────────────────────────────────────
 * Layout v2 (little endian):
 *   u8 version | u8 state | u8 reason | u8 forced | i64 since_utc |
 *   u8 hist_n  | PP_HIST_MAX × { i64 utc | u32 boot } |
 *   u8 probation | u8 probe_fails | u8 has_low | i64 low_utc | u32 gen | u32 crc32(all above)
 * PP_BLOB_LEN = 128 B. v1 (113 B, the first release) is the same without the
 * 15 recovery bytes; it reads as generation 0. It
 * is still read, so an upgrade keeps every stored state.
 * Explicit bytes, not a struct dump: padding and field order can never make a
 * blob written by one build unreadable by the next, and a torn or foreign
 * value fails the CRC instead of being trusted. */

static uint32_t crc32_ieee(const uint8_t *b, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put_u32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
}

static void put_i64(uint8_t *b, int64_t v)
{
    const uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(u >> (8 * i));
}

static uint32_t get_u32(const uint8_t *b)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)b[i] << (8 * i);
    return v;
}

static int64_t get_i64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[i] << (8 * i);
    return (int64_t)v;
}

size_t pp_encode(const pp_persist_t *p, uint8_t *buf, size_t cap)
{
    if (p == NULL || buf == NULL || cap < PP_BLOB_LEN) return 0;
    memset(buf, 0, PP_BLOB_LEN);
    size_t o = 0;
    buf[o++] = PP_BLOB_VERSION;
    buf[o++] = p->state;
    buf[o++] = p->reason;
    buf[o++] = p->forced ? 1 : 0;
    put_i64(buf + o, p->since_utc); o += 8;
    const uint8_t n = p->hist_n > PP_HIST_MAX ? PP_HIST_MAX : p->hist_n;
    buf[o++] = n;
    for (unsigned i = 0; i < PP_HIST_MAX; i++) {
        /* Unused slots are written as zeros so equal states encode equally. */
        const pp_brownout_t e = i < n ? p->hist[i] : (pp_brownout_t){0, 0};
        put_i64(buf + o, e.utc); o += 8;
        put_u32(buf + o, e.boot); o += 4;
    }
    buf[o++] = p->probation;
    buf[o++] = p->probe_fails;
    buf[o++] = p->has_low ? 1 : 0;
    put_i64(buf + o, p->has_low ? p->low_utc : 0); o += 8;
    put_u32(buf + o, p->gen); o += 4;
    put_u32(buf + o, crc32_ieee(buf, o)); o += 4;
    return o;
}

bool pp_decode(pp_persist_t *out, const uint8_t *buf, size_t len)
{
    if (out == NULL || buf == NULL) return false;
    if (!(len == PP_BLOB_LEN && buf[0] == PP_BLOB_VERSION) && !(len == PP_BLOB_V1_LEN && buf[0] == 1u)) {
        return false;
    }
    if (get_u32(buf + len - 4) != crc32_ieee(buf, len - 4)) return false;
    pp_persist_t p;
    memset(&p, 0, sizeof p);
    size_t o = 1;
    p.state = buf[o++];
    p.reason = buf[o++];
    const uint8_t forced = buf[o++];
    p.since_utc = get_i64(buf + o); o += 8;
    p.hist_n = buf[o++];
    if (p.state > PP_STATE_HOLD || p.reason >= PP_REASON_COUNT || forced > 1 ||
        p.hist_n > PP_HIST_MAX) {
        return false;
    }
    /* A forced state is only ever no_flash or hold. */
    if (forced && p.state == PP_STATE_NORMAL) return false;
    p.forced = forced != 0;
    for (unsigned i = 0; i < PP_HIST_MAX; i++) {
        p.hist[i].utc = get_i64(buf + o); o += 8;
        p.hist[i].boot = get_u32(buf + o); o += 4;
        if (i >= p.hist_n) p.hist[i] = (pp_brownout_t){0, 0};
    }
    if (len == PP_BLOB_LEN) {
        p.probation = buf[o++];
        p.probe_fails = buf[o++];
        const uint8_t has_low = buf[o++];
        p.low_utc = get_i64(buf + o); o += 8;
        if (p.probation > PP_PROBATION_NORMAL || p.probe_fails > PP_PROBE_FAILS_MAX || has_low > 1) return false;
        if (!has_low && p.low_utc != 0) return false;
        p.has_low = has_low != 0;
        p.gen = get_u32(buf + o); o += 4;
        /* A probation is always automatic and in the state its step entered. */
        if (p.probation != PP_PROBATION_NONE &&
            (p.forced || p.state != (p.probation == PP_PROBATION_NO_FLASH ? PP_STATE_NO_FLASH
                                                                          : PP_STATE_NORMAL))) {
            return false;
        }
    }
    *out = p;
    return true;
}

/* ── shared helpers ────────────────────────────────────────────────────── */

static pp_event_t no_event(const pp_core_t *c)
{
    const pp_state_t s = (pp_state_t)c->p.state;
    return (pp_event_t){ .changed = false, .persist = false, .from = s, .to = s,
                         .reason = (pp_reason_t)c->p.reason };
}

/* ── accumulated exit evidence (RAM) ─────────────────────────────────────── */

static void acc_reset(pp_core_t *c)
{
    memset(c->acc_ms, 0, sizeof c->acc_ms);
    memset(c->acc_bucket, 0, sizeof c->acc_bucket);
    c->prev_qual = false;
}

static void acc_credit(pp_core_t *c, int64_t mono_ms, int64_t ms)
{
    const int64_t b = mono_ms / PP_EXIT_BUCKET_MS;
    const unsigned s = (unsigned)(b % PP_EXIT_BUCKETS);
    if (c->acc_bucket[s] != b) {   /* a slot from an older lap of the ring */
        c->acc_bucket[s] = b;
        c->acc_ms[s] = 0;
    }
    c->acc_ms[s] += (uint32_t)ms;  /* ≤ 60 s per credit, ≤ 30 min + 60 s per slot */
}

/* Evidence inside the horizon: the PP_EXIT_BUCKETS newest buckets. */
static int64_t acc_sum(const pp_core_t *c, int64_t mono_ms)
{
    const int64_t b = mono_ms / PP_EXIT_BUCKET_MS;
    int64_t sum = 0;
    for (unsigned s = 0; s < PP_EXIT_BUCKETS; s++) {
        if (c->acc_ms[s] != 0 && c->acc_bucket[s] <= b && c->acc_bucket[s] > b - (int64_t)PP_EXIT_BUCKETS) {
            sum += c->acc_ms[s];
        }
    }
    return sum;
}

/* ms until the newest evidence leaves the horizon; 0 when there is none. */
static int64_t acc_expiry_ms(const pp_core_t *c, int64_t mono_ms)
{
    const int64_t b = mono_ms / PP_EXIT_BUCKET_MS;
    int64_t newest = -1;
    for (unsigned s = 0; s < PP_EXIT_BUCKETS; s++) {
        if (c->acc_ms[s] != 0 && c->acc_bucket[s] <= b && c->acc_bucket[s] > b - (int64_t)PP_EXIT_BUCKETS &&
            c->acc_bucket[s] > newest) {
            newest = c->acc_bucket[s];
        }
    }
    if (newest < 0) return 0;
    const int64_t left = (newest + (int64_t)PP_EXIT_BUCKETS) * PP_EXIT_BUCKET_MS - mono_ms;
    return left > 0 ? left : 0;
}

/* Move to `to` for `why`. since_utc moves only when the STATE moves (a forced
 * relabel of the current state keeps the original entry time). Every
 * transition ends a probation and restarts the accumulated evidence; the
 * probe sets its probation after this call. */
static pp_event_t transition(pp_core_t *c, pp_state_t to, pp_reason_t why, pp_clock_t clk)
{
    pp_event_t ev = no_event(c);
    ev.to = to;
    ev.reason = why;
    ev.changed = (pp_state_t)c->p.state != to;
    ev.persist = true;
    if (ev.changed) c->p.since_utc = clk.valid ? clk.utc : 0;
    c->p.state = (uint8_t)to;
    c->p.reason = (uint8_t)why;
    c->p.probation = PP_PROBATION_NONE;
    c->p.has_low = false;   /* the new since_utc is later than any earlier low */
    c->p.low_utc = 0;
    acc_reset(c);
    return ev;
}

/* ── battery-only probe ────────────────────────────────────────────────── */

static int64_t probe_wait_s(uint8_t fails)
{
    const unsigned level = fails > PP_PROBE_LEVEL_CAP ? PP_PROBE_LEVEL_CAP : fails;
    return PP_PROBE_QUIET_BASE_S << level;
}

/* Seconds without a brownout or a fresh low read, as far as they can be
 * proven. With a valid clock, a dated entry (since_utc) and a dated newest
 * brownout: since the later of the two, across reboots. Otherwise the time
 * since boot or since this boot's last transition, whichever is later: every
 * brownout reboots, so uptime is a lower bound on the time since the last one
 * (any reboot restarts it: conservative). A fresh low read this boot caps it
 * as well. */
static int64_t quiet_s(const pp_core_t *c, int64_t mono_ms, pp_clock_t clk)
{
    const pp_brownout_t *last = c->p.hist_n > 0 ? &c->p.hist[c->p.hist_n - 1] : NULL;
    /* The durable low: exact while this boot's RAM copy (have_low) caps the
     * result below; after a reboot it counts PP_LOW_REFRESH_S later than
     * written, since it may be that stale. Undated: uptime only. */
    const int64_t low_anchor = c->p.has_low && c->p.low_utc != 0
        ? c->p.low_utc + (c->have_low ? 0 : PP_LOW_REFRESH_S) : 0;
    int64_t q;
    if (clk.valid && c->p.since_utc != 0 && clk.utc >= c->p.since_utc &&
        (last == NULL || (last->utc != 0 && clk.utc >= last->utc)) &&
        (!c->p.has_low || (c->p.low_utc != 0 && clk.utc >= c->p.low_utc))) {
        int64_t anchor = c->p.since_utc;
        if (last != NULL && last->utc > anchor) anchor = last->utc;
        if (low_anchor > anchor) anchor = low_anchor;
        q = clk.utc - anchor;
    } else {
        q = (mono_ms - c->since_mono_ms) / 1000;
    }
    if (c->have_low) {
        const int64_t lq = (mono_ms - c->last_low_ms) / 1000;
        if (lq < q) q = lq;
    }
    return q < 0 ? 0 : q;
}

/* Seconds on probation: by UTC from the step (since_utc) when it is dated and
 * the clock is valid, else since the step in this boot, or since boot. */
static int64_t probation_s(const pp_core_t *c, int64_t mono_ms, pp_clock_t clk)
{
    if (clk.valid && c->p.since_utc != 0 && clk.utc >= c->p.since_utc) return clk.utc - c->p.since_utc;
    const int64_t s = (mono_ms - c->since_mono_ms) / 1000;
    return s < 0 ? 0 : s;
}

static pp_event_t probe_fail(pp_core_t *c, pp_state_t to, pp_clock_t clk)
{
    const uint8_t fails = c->p.probe_fails < PP_PROBE_FAILS_MAX ? (uint8_t)(c->p.probe_fails + 1u)
                                                                : (uint8_t)PP_PROBE_FAILS_MAX;
    const pp_event_t ev = transition(c, to, PP_REASON_PROBE_FAILED, clk);
    c->p.probe_fails = fails;
    return ev;
}

static pp_event_t probe_pass(pp_core_t *c)
{
    /* The state stays (reason probe); the probation flag goes. Persisted so a
     * later brownout is a new episode, not a failed probe. The backoff level
     * stays: only a qualifying exit or clear proves the power good again. */
    c->p.probation = PP_PROBATION_NONE;
    pp_event_t ev = no_event(c);
    ev.persist = true;
    ev.probe_passed = true;
    ev.relaxes = true;
    return ev;
}

/* Record a fresh low read durably: the first of this state, a first dated
 * one, or a refresh at most every PP_LOW_REFRESH_S. True = persist. */
static bool record_low(pp_core_t *c, pp_clock_t clk)
{
    const int64_t now = clk.valid ? clk.utc : 0;
    bool write = false;
    if (!c->p.has_low) {
        write = true;
    } else if (c->p.low_utc == 0) {
        write = now != 0;                              /* now it can be dated */
    } else if (now == 0) {
        /* A dated low, and now a low the clock cannot date (hwm, a suspect
         * RTC): the dated record is older than the newest low by an unknown
         * amount. Switch to undated, so a reboot counts the quiet period from
         * boot rather than from the old date (review of PR #113, round 2). */
        write = true;
    } else {
        write = now < c->p.low_utc || now - c->p.low_utc >= PP_LOW_REFRESH_S;
    }
    if (write) {
        c->p.has_low = true;
        c->p.low_utc = now;
    }
    return write;
}

pp_event_t pp_on_power_loss(pp_core_t *c, pp_clock_t clk)
{
    if (c->p.probation == PP_PROBATION_NONE) return no_event(c);
    /* The probation ended with the power gone: as much a failed probe as a
     * brownout (and the RTC copy of anything unwritten is gone with it). */
    return probe_fail(c, PP_STATE_HOLD, clk);
}

pp_link_t pp_brownout_link(const pp_brownout_t *prev, const pp_brownout_t *cur)
{
    if (prev == NULL || cur == NULL) return PP_LINK_NO;
    if (prev->utc != 0 && cur->utc != 0 && cur->utc >= prev->utc) {
        /* Both clocks valid and ordered: UTC alone decides, both ways. */
        return cur->utc - prev->utc < PP_BROWNOUT_LINK_S ? PP_LINK_YES : PP_LINK_NO;
    }
    /* Clock-invalid fallback (also a clock that went backwards between the
     * two, which proves one of them wrong): "the previous boot also ended in a
     * brownout". The Uluru 15 Nov sequence (boots 2, 3, 4) links either way. */
    if (prev->boot != 0 && cur->boot != 0) {
        return cur->boot == prev->boot + 1u ? PP_LINK_YES : PP_LINK_NO;
    }
    /* diag's boot counter can legitimately be 0 (NVS unavailable). With no
     * clock either, nothing orders the two brownouts. */
    return PP_LINK_UNORDERED;
}

/* ── inputs ────────────────────────────────────────────────────────────── */

pp_event_t pp_on_boot(pp_core_t *c, bool brownout_reset, uint32_t boot, pp_clock_t clk)
{
    /* A reboot always restarts the accumulated evidence: it is RAM only. */
    acc_reset(c);
    c->have_read = false;
    c->have_low = false;
    c->monitor_live = false;
    c->since_mono_ms = 0;   /* a state from an earlier boot: uptime counts from boot */
    if (!brownout_reset) return no_event(c);

    const pp_brownout_t cur = { .utc = clk.valid ? clk.utc : 0, .boot = boot };
    const pp_brownout_t *prev = c->p.hist_n > 0 ? &c->p.hist[c->p.hist_n - 1] : NULL;
    if (prev != NULL && boot != 0 && prev->boot == boot) {
        return no_event(c);   /* this boot's brownout is already recorded */
    }
    const pp_link_t link = pp_brownout_link(prev, &cur);

    if (c->p.hist_n == PP_HIST_MAX) {
        memmove(&c->p.hist[0], &c->p.hist[1], (PP_HIST_MAX - 1) * sizeof c->p.hist[0]);
        c->p.hist_n--;
    }
    c->p.hist[c->p.hist_n++] = cur;

    const pp_state_t from = (pp_state_t)c->p.state;
    pp_event_t ev;
    if (c->p.probation != PP_PROBATION_NONE) {
        /* A brownout on probation: the probe failed. It belongs to the episode
         * the probe stepped out of, so it escalates like a linked one (hold),
         * and the next probe waits twice as long. */
        ev = probe_fail(c, PP_STATE_HOLD, clk);
    } else if (link == PP_LINK_YES && from != PP_STATE_HOLD) {
        /* A second brownout in one episode: escalate, from no_flash and also
         * straight from normal (a unit that had recovered and browns out again
         * within 6 h has shown its power cannot carry the reduced load either).
         * A forced no_flash escalates too; the forced flag stays. */
        ev = transition(c, PP_STATE_HOLD, PP_REASON_REPEAT_BROWNOUT, clk);
    } else if (link == PP_LINK_UNORDERED && from == PP_STATE_NO_FLASH) {
        /* Already protected after an earlier brownout, and nothing can tell
         * whether this one belongs to the same episode: assume it does. */
        ev = transition(c, PP_STATE_HOLD, PP_REASON_BROWNOUT_UNORDERED, clk);
    } else if (from == PP_STATE_NORMAL) {
        ev = transition(c, PP_STATE_NO_FLASH, PP_REASON_BROWNOUT, clk);
    } else {
        /* Already protected at least as far as this brownout asks: the state
         * and its reason stay, the history entry must still be persisted. */
        ev = no_event(c);
    }
    ev.persist = true;
    return ev;
}

static pp_event_t on_read(pp_core_t *c, power_read_reason_t reason, const power_reading_t *p,
                          bool fresh_present, int64_t mono_ms, pp_clock_t clk)
{
    /* Only a valid read with input present whose frames are changing is
     * evidence about the rail; stale, invalid and no_input reads never are. */
    const bool fresh = reason == POWER_READ_OK && p != NULL && p->input_present && fresh_present;
    const bool in_seq = c->have_read && mono_ms >= c->last_read_ms &&
                        mono_ms - c->last_read_ms <= PP_READ_GAP_MAX_MS;
    const int64_t interval = in_seq ? mono_ms - c->last_read_ms : 0;
    c->have_read = true;
    c->last_read_ms = mono_ms;
    /* A charger that answered: the monitor the probe needs (fail-closed
     * without one). no_input is a live answer, the battery-only case itself. */
    c->monitor_live = reason != POWER_READ_UNAVAILABLE && reason != POWER_READ_I2C_ERROR;

    /* battery_v == 0 with input: the charger idles and the unit runs on the
     * battery through the BATFET, so the rail IS the battery (C in the
     * header). The EXIT judges it with the battery threshold. The entry
     * trigger (also the probation-failure test) keeps the first release's
     * rule: a 0 never triggers on its own. Uluru units idle on 3.3-3.6 V
     * rails, and entering there would cut Fv/Fm data mid-deployment. */
    const uint32_t sys = fresh ? p->system_mv : 0;
    const uint32_t bat_raw = fresh ? p->battery_mv : 0;
    const uint32_t bat_exit = bat_raw != 0 ? bat_raw : sys;
    const bool qualifies = fresh && sys >= PP_SYS_EXIT_MV && bat_exit >= PP_BAT_EXIT_MV;
    const bool low = fresh && !qualifies;
    const bool trigger = fresh && (sys < PP_SYS_TRIGGER_MV ||
                                   (bat_raw > 0 && bat_raw < PP_BAT_TRIGGER_MV));

    /* A relaxation whose commit failed waits (the glue set the hold). */
    const bool may_relax = mono_ms >= c->relax_hold_until_ms;
    const pp_state_t s = (pp_state_t)c->p.state;
    if (s == PP_STATE_NORMAL) {
        acc_reset(c);
        if (c->p.probation == PP_PROBATION_NORMAL) {
            if (trigger) return probe_fail(c, PP_STATE_NO_FLASH, clk);
            pp_event_t ev = no_event(c);
            if (may_relax && probation_s(c, mono_ms, clk) >= PP_PROBATION_S) ev.relax_due = PP_RELAX_PASS;
            return ev;
        }
        if (trigger) return transition(c, PP_STATE_NO_FLASH, PP_REASON_LOW_VOLTAGE, clk);
        return no_event(c);
    }
    if (c->p.forced) {
        acc_reset(c);   /* a forced state never auto-exits, never probes */
        return no_event(c);
    }

    if (qualifies) {
        /* Only the interval between two consecutive qualifying reads counts. */
        if (c->prev_qual && in_seq) acc_credit(c, mono_ms, interval);
        c->prev_qual = true;
        if (may_relax && acc_sum(c, mono_ms) >= PP_EXIT_TARGET_MS) {
            pp_event_t ev = no_event(c);
            ev.relax_due = PP_RELAX_EXIT;
            return ev;
        }
    }
    bool low_recorded = false;
    if (low) {
        /* Fresh, with input, and not good enough: the battery is not there
         * yet. Everything collected goes, and the probe waits a full quiet
         * period from here, durably (record_low: never per read). */
        acc_reset(c);
        c->have_low = true;
        c->last_low_ms = mono_ms;
        low_recorded = record_low(c, clk);
    } else if (!qualifies) {
        c->prev_qual = false;   /* no evidence either way: pause */
    }
    pp_event_t quiet = no_event(c);
    quiet.persist = low_recorded;

    if (c->p.probation == PP_PROBATION_NO_FLASH) {
        if (trigger) return probe_fail(c, PP_STATE_HOLD, clk);
        /* The pass is only reported due: the read's low record (quiet.persist)
         * stands on its own, whether or not the pass is then committed. */
        if (may_relax && probation_s(c, mono_ms, clk) >= PP_PROBATION_S) quiet.relax_due = PP_RELAX_PASS;
        return quiet;
    }
    /* The probe is for the unit WITHOUT evidence. While qualifying evidence
     * sits inside the horizon (or this read qualifies) the accumulation
     * decides; once it has expired the probe may run again. */
    if (qualifies || acc_sum(c, mono_ms) > 0) return quiet;
    if (may_relax && c->monitor_live && c->p.probe_fails < PP_PROBE_FAILS_MAX &&
        quiet_s(c, mono_ms, clk) >= probe_wait_s(c->p.probe_fails)) {
        quiet.relax_due = PP_RELAX_PROBE;
    }
    return quiet;
}

pp_event_t pp_relax(pp_core_t *c, pp_relax_t what, int64_t mono_ms, pp_clock_t clk)
{
    const pp_state_t s = (pp_state_t)c->p.state;
    pp_event_t ev = no_event(c);
    switch (what) {
    case PP_RELAX_EXIT:
        if (s == PP_STATE_NORMAL || c->p.forced) return ev;
        ev = transition(c, PP_STATE_NORMAL, PP_REASON_POWER_RECOVERED, clk);
        c->p.probe_fails = 0;   /* proven good power: the backoff starts over */
        break;
    case PP_RELAX_PROBE: {
        if (s == PP_STATE_NORMAL || c->p.forced || c->p.probation != PP_PROBATION_NONE) return ev;
        /* One level down, on probation: steady-state work comes back before
         * the saturating flashes do. */
        const pp_state_t to = s == PP_STATE_HOLD ? PP_STATE_NO_FLASH : PP_STATE_NORMAL;
        ev = transition(c, to, PP_REASON_PROBE, clk);
        c->p.probation = to == PP_STATE_NO_FLASH ? PP_PROBATION_NO_FLASH : PP_PROBATION_NORMAL;
        break;
    }
    case PP_RELAX_PASS:
        if (c->p.probation == PP_PROBATION_NONE) return ev;
        ev = probe_pass(c);
        break;
    case PP_RELAX_NONE:
    default:
        return ev;
    }
    if (ev.changed) c->since_mono_ms = mono_ms;
    ev.relaxes = true;
    return ev;
}

pp_event_t pp_on_read(pp_core_t *c, power_read_reason_t reason, const power_reading_t *p,
                      bool fresh_present, int64_t mono_ms, pp_clock_t clk)
{
    const pp_event_t ev = on_read(c, reason, p, fresh_present, mono_ms, clk);
    /* The uptime fallbacks (quiet period, probation) count from here. */
    if (ev.changed) c->since_mono_ms = mono_ms;
    return ev;
}

pp_event_t pp_force(pp_core_t *c, pp_state_t state, pp_clock_t clk)
{
    if (state != PP_STATE_NO_FLASH && state != PP_STATE_HOLD) return no_event(c);
    if ((pp_state_t)c->p.state == state && c->p.forced) return no_event(c);
    /* Forcing a LOWER state (hold -> no_flash) lowers protection: like every
     * relaxation it must be on record before it takes effect. */
    const bool lower = state < (pp_state_t)c->p.state;
    c->p.forced = true;
    pp_event_t ev = transition(c, state, PP_REASON_FORCED, clk);
    ev.relaxes = lower;
    return ev;
}

pp_event_t pp_clear(pp_core_t *c, pp_clock_t clk)
{
    if ((pp_state_t)c->p.state == PP_STATE_NORMAL && !c->p.forced && c->p.hist_n == 0 &&
        c->p.probation == PP_PROBATION_NONE && c->p.probe_fails == 0) {
        return no_event(c);   /* nothing to clear: no NVS write */
    }
    c->p.forced = false;
    c->p.hist_n = 0;
    memset(c->p.hist, 0, sizeof c->p.hist);
    c->p.probe_fails = 0;
    pp_event_t ev = transition(c, PP_STATE_NORMAL, PP_REASON_CLEARED, clk);
    ev.relaxes = true;   /* everything it wipes is protection */
    return ev;
}

/* ── reporting ─────────────────────────────────────────────────────────── */

uint32_t pp_brownouts_6h(const pp_core_t *c, pp_clock_t clk, uint32_t cur_boot)
{
    /* chain[i]: entry i is in the run of consecutive brownout boots that ends
     * at THIS boot (newest entry == cur_boot, then cur_boot-1, ...). */
    bool chain[PP_HIST_MAX] = { false };
    uint32_t want = cur_boot;
    for (int i = (int)c->p.hist_n - 1; i >= 0 && want != 0; i--) {
        if (c->p.hist[i].boot != want) break;
        chain[i] = true;
        want--;
    }
    uint32_t n = 0;
    for (unsigned i = 0; i < c->p.hist_n; i++) {
        const pp_brownout_t *e = &c->p.hist[i];
        /* Same precedence as pp_brownout_link: UTC decides whenever both sides
         * have a valid, ordered one; the boot chain is the fallback; an entry
         * neither can date counts (conservative). */
        if (clk.valid && e->utc != 0 && clk.utc >= e->utc) {
            if (clk.utc - e->utc < PP_BROWNOUT_LINK_S) n++;
        } else if (e->boot != 0 && cur_boot != 0) {
            if (chain[i]) n++;
        } else {
            n++;
        }
    }
    return n;
}

int64_t pp_exit_remaining_ms(const pp_core_t *c, int64_t mono_ms)
{
    if ((pp_state_t)c->p.state == PP_STATE_NORMAL || c->p.forced) return -1;
    const int64_t have = acc_sum(c, mono_ms);
    if (have <= 0) return -1;
    return have >= PP_EXIT_TARGET_MS ? 0 : PP_EXIT_TARGET_MS - have;
}

pp_recovery_t pp_recovery(const pp_core_t *c, int64_t mono_ms, pp_clock_t clk)
{
    pp_recovery_t r = { .qualifying_s = 0, .probe = PP_PROBE_OFF, .next_probe_s = -1,
                        .backoff_level = c->p.probe_fails };
    const pp_state_t s = (pp_state_t)c->p.state;
    if (s != PP_STATE_NORMAL && !c->p.forced) r.qualifying_s = (uint32_t)(acc_sum(c, mono_ms) / 1000);
    if (c->p.probation != PP_PROBATION_NONE) {
        r.probe = PP_PROBE_PROBATION;
    } else if (s == PP_STATE_NORMAL || c->p.forced) {
        r.probe = PP_PROBE_OFF;
    } else if (c->p.probe_fails >= PP_PROBE_FAILS_MAX) {
        r.probe = PP_PROBE_EXHAUSTED;
    } else if (!c->monitor_live) {
        r.probe = PP_PROBE_NO_MONITOR;
    } else {
        r.probe = PP_PROBE_WAITING;
        int64_t left = probe_wait_s(c->p.probe_fails) - quiet_s(c, mono_ms, clk);
        const int64_t evidence_s = (acc_expiry_ms(c, mono_ms) + 999) / 1000;
        if (evidence_s > left) left = evidence_s;   /* collected evidence blocks the probe */
        const int64_t hold_s = (c->relax_hold_until_ms - mono_ms + 999) / 1000;
        if (hold_s > left) left = hold_s;           /* a failed commit waits */
        r.next_probe_s = left > 0 ? left : 0;
    }
    return r;
}

int pp_render_json(char *buf, size_t cap, const pp_persist_t *p, bool persist_fault,
                   uint32_t brownouts_6h, const pp_skips_t *skips, const pp_recovery_t *rec)
{
    if (buf == NULL || cap == 0 || p == NULL) return -1;
    const pp_skips_t z = {0, 0, 0};
    if (skips == NULL) skips = &z;
    const pp_recovery_t none = { 0, PP_PROBE_OFF, -1, 0 };
    if (rec == NULL) rec = &none;
    char since[24] = "null";
    if (p->since_utc != 0) snprintf(since, sizeof since, "%lld", (long long)p->since_utc);
    char next[24] = "null";
    if (rec->next_probe_s >= 0) snprintf(next, sizeof next, "%lld", (long long)rec->next_probe_s);
    /* Every string is a fixed enum name: no escaping needed. */
    const int n = snprintf(buf, cap,
        "{\"state\":\"%s\",\"reason\":\"%s\",\"since_utc\":%s,\"forced\":%s,"
        "\"persist_fault\":%s,\"brownouts_6h\":%lu,"
        "\"qualifying_s\":%lu,\"probe\":\"%s\",\"next_probe_s\":%s,\"backoff_level\":%u,"
        "\"skipped\":{\"flash_runs\":%lu,\"ambit_runs\":%lu,\"ota\":%lu}}",
        pp_state_name((pp_state_t)p->state), pp_reason_name((pp_reason_t)p->reason), since,
        p->forced ? "true" : "false", persist_fault ? "true" : "false", (unsigned long)brownouts_6h,
        (unsigned long)rec->qualifying_s, pp_probe_name(rec->probe), next, (unsigned)rec->backoff_level,
        (unsigned long)skips->flash_runs, (unsigned long)skips->ambit_runs,
        (unsigned long)skips->ota);
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return -1;
    }
    return n;
}
