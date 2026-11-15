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
    case PP_REASON_COUNT:           break;
    }
    return "none";
}

bool pp_force_state_parse(const char *s, pp_state_t *out)
{
    if (s == NULL) return false;
    if (strcmp(s, "no_flash") == 0) { *out = PP_STATE_NO_FLASH; return true; }
    if (strcmp(s, "hold") == 0)     { *out = PP_STATE_HOLD;     return true; }
    return false;
}

/* ── blob codec ────────────────────────────────────────────────────────────
 * Layout (little endian, PP_BLOB_LEN = 113 B):
 *   u8 version | u8 state | u8 reason | u8 forced | i64 since_utc |
 *   u8 hist_n  | PP_HIST_MAX × { i64 utc | u32 boot } | u32 crc32(all above)
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
    put_u32(buf + o, crc32_ieee(buf, o)); o += 4;
    return o;
}

bool pp_decode(pp_persist_t *out, const uint8_t *buf, size_t len)
{
    if (out == NULL || buf == NULL || len != PP_BLOB_LEN) return false;
    if (buf[0] != PP_BLOB_VERSION) return false;
    if (get_u32(buf + PP_BLOB_LEN - 4) != crc32_ieee(buf, PP_BLOB_LEN - 4)) return false;
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

static void window_reset(pp_core_t *c)
{
    c->window_open = false;
    c->window_start_ms = 0;
}

/* Move to `to` for `why`. since_utc moves only when the STATE moves (a forced
 * relabel of the current state keeps the original entry time). */
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
    window_reset(c);
    return ev;
}

bool pp_brownouts_linked(const pp_brownout_t *prev, const pp_brownout_t *cur)
{
    if (prev == NULL || cur == NULL) return false;
    if (prev->utc != 0 && cur->utc != 0 && cur->utc >= prev->utc) {
        /* Both clocks valid and ordered: UTC alone decides, both ways. */
        return cur->utc - prev->utc < PP_BROWNOUT_LINK_S;
    }
    /* Clock-invalid fallback (also a clock that went backwards between the
     * two, which proves one of them wrong): "the previous boot also ended in a
     * brownout". The Uluru 15 Nov sequence (boots 2, 3, 4) links either way. */
    return prev->boot != 0 && cur->boot != 0 && cur->boot == prev->boot + 1u;
}

/* ── inputs ────────────────────────────────────────────────────────────── */

pp_event_t pp_on_boot(pp_core_t *c, bool brownout_reset, uint32_t boot, pp_clock_t clk)
{
    /* A reboot always restarts the exit window: it is RAM-only evidence. */
    window_reset(c);
    c->have_read = false;
    if (!brownout_reset) return no_event(c);

    const pp_brownout_t cur = { .utc = clk.valid ? clk.utc : 0, .boot = boot };
    const pp_brownout_t *prev = c->p.hist_n > 0 ? &c->p.hist[c->p.hist_n - 1] : NULL;
    if (prev != NULL && boot != 0 && prev->boot == boot) {
        return no_event(c);   /* this boot's brownout is already recorded */
    }
    const bool linked = pp_brownouts_linked(prev, &cur);

    if (c->p.hist_n == PP_HIST_MAX) {
        memmove(&c->p.hist[0], &c->p.hist[1], (PP_HIST_MAX - 1) * sizeof c->p.hist[0]);
        c->p.hist_n--;
    }
    c->p.hist[c->p.hist_n++] = cur;

    const pp_state_t from = (pp_state_t)c->p.state;
    pp_event_t ev;
    if (linked && from != PP_STATE_HOLD) {
        /* A second brownout in one episode: escalate, from no_flash and also
         * straight from normal (a unit that had recovered and browns out again
         * within 6 h has shown its power cannot carry the reduced load either).
         * A forced no_flash escalates too; the forced flag stays. */
        ev = transition(c, PP_STATE_HOLD, PP_REASON_REPEAT_BROWNOUT, clk);
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

pp_event_t pp_on_read(pp_core_t *c, power_read_reason_t reason, const power_reading_t *p,
                      bool fresh_present, int64_t mono_ms, pp_clock_t clk)
{
    /* Only a valid read with input present whose frames are changing is
     * evidence about the rail; stale, invalid and no_input reads never are. */
    const bool fresh = reason == POWER_READ_OK && p != NULL && p->input_present && fresh_present;
    const bool gap = c->have_read &&
                     (mono_ms < c->last_read_ms || mono_ms - c->last_read_ms > PP_READ_GAP_MAX_MS);
    c->have_read = true;
    c->last_read_ms = mono_ms;

    const pp_state_t s = (pp_state_t)c->p.state;
    if (s == PP_STATE_NORMAL) {
        window_reset(c);
        if (fresh && (p->system_mv < PP_SYS_TRIGGER_MV ||
                      (p->battery_mv > 0 && p->battery_mv < PP_BAT_TRIGGER_MV))) {
            /* battery_mv == 0 with input = no battery or an idle charger
             * (power-telemetry.md): never a trigger on its own. */
            return transition(c, PP_STATE_NO_FLASH, PP_REASON_LOW_VOLTAGE, clk);
        }
        return no_event(c);
    }
    if (c->p.forced) {
        window_reset(c);   /* a forced state never auto-exits */
        return no_event(c);
    }

    const bool qualifies = fresh && p->system_mv >= PP_SYS_EXIT_MV &&
                           (p->battery_mv >= PP_BAT_EXIT_MV || p->battery_mv == 0);
    if (!qualifies) {
        window_reset(c);
        return no_event(c);
    }
    if (!c->window_open || gap) {
        c->window_open = true;
        c->window_start_ms = mono_ms;
        return no_event(c);
    }
    if (mono_ms - c->window_start_ms >= PP_EXIT_WINDOW_MS) {
        return transition(c, PP_STATE_NORMAL, PP_REASON_POWER_RECOVERED, clk);
    }
    return no_event(c);
}

pp_event_t pp_force(pp_core_t *c, pp_state_t state, pp_clock_t clk)
{
    if (state != PP_STATE_NO_FLASH && state != PP_STATE_HOLD) return no_event(c);
    if ((pp_state_t)c->p.state == state && c->p.forced) return no_event(c);
    c->p.forced = true;
    return transition(c, state, PP_REASON_FORCED, clk);
}

pp_event_t pp_clear(pp_core_t *c, pp_clock_t clk)
{
    if ((pp_state_t)c->p.state == PP_STATE_NORMAL && !c->p.forced && c->p.hist_n == 0) {
        return no_event(c);   /* nothing to clear: no NVS write */
    }
    c->p.forced = false;
    c->p.hist_n = 0;
    memset(c->p.hist, 0, sizeof c->p.hist);
    return transition(c, PP_STATE_NORMAL, PP_REASON_CLEARED, clk);
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
        /* Same precedence as pp_brownouts_linked: UTC decides whenever both
         * sides have a valid, ordered one; the boot chain is the fallback. */
        const bool by_utc = clk.valid && e->utc != 0 && clk.utc >= e->utc;
        if (by_utc ? clk.utc - e->utc < PP_BROWNOUT_LINK_S : chain[i]) n++;
    }
    return n;
}

int64_t pp_window_remaining_ms(const pp_core_t *c, int64_t mono_ms)
{
    if (!c->window_open || mono_ms < c->window_start_ms) return -1;
    const int64_t left = PP_EXIT_WINDOW_MS - (mono_ms - c->window_start_ms);
    return left > 0 ? left : 0;
}

int pp_render_json(char *buf, size_t cap, const pp_persist_t *p, uint32_t brownouts_6h,
                   const pp_skips_t *skips)
{
    if (buf == NULL || cap == 0 || p == NULL) return -1;
    const pp_skips_t z = {0, 0, 0};
    if (skips == NULL) skips = &z;
    char since[24] = "null";
    if (p->since_utc != 0) snprintf(since, sizeof since, "%lld", (long long)p->since_utc);
    /* Every string is a fixed enum name: no escaping needed. */
    const int n = snprintf(buf, cap,
        "{\"state\":\"%s\",\"reason\":\"%s\",\"since_utc\":%s,\"forced\":%s,"
        "\"brownouts_6h\":%lu,\"skipped\":{\"flash_runs\":%lu,\"ambit_runs\":%lu,\"ota\":%lu}}",
        pp_state_name((pp_state_t)p->state), pp_reason_name((pp_reason_t)p->reason), since,
        p->forced ? "true" : "false", (unsigned long)brownouts_6h,
        (unsigned long)skips->flash_runs, (unsigned long)skips->ambit_runs,
        (unsigned long)skips->ota);
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return -1;
    }
    return n;
}
