/* Host harness: the production MP2731 read (components/mp2731/mp2731_core.c) and
 * power policy (components/device_commands/power_policy.c) driven by a fake
 * charger behind the register seam. tests/test_power_policy.py runs every
 * scenario; built with -DLEGACY_IMPL the two incident scenarios run the v2.16.0
 * code instead (tests/fixtures/power_v2_16_0) and must FAIL - the red half of
 * the red/green reproduction of the 2026-11-13 Uluru incident.
 *
 * usage: power_policy_host <scenario>   (exit 0 = pass) */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp2731_core.h"
#include "power_policy.h"

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

/* ── Fake MP2731 ─────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t r[0x20];
    bool    adc_available;  /* charge/OTG mode: conversions update results */
    bool    edge_only;      /* a conversion starts only on a 0→1 START edge */
    int     latency_reads;  /* REG03 reads with START high before it clears; -1 never */
    int     reads_left;
    bool    vary;           /* healthy: every conversion produces new numbers */
    bool    i2c_fail;
    int     conversions;
    int     writes03;
    uint32_t slept_ms;
} chip_t;

static esp_err_t chip_read(void *ctx, uint8_t reg, uint8_t *val)
{
    chip_t *c = ctx;
    if (c->i2c_fail) return ESP_FAIL;
    if (reg == MP2731_REG_ADC_CTRL && (c->r[reg] & MP2731_ADC_START_BIT) &&
        c->reads_left > 0 && --c->reads_left == 0) {
        c->r[reg] &= (uint8_t)~MP2731_ADC_START_BIT;     /* conversion done */
    }
    *val = c->r[reg];
    return ESP_OK;
}

static esp_err_t chip_write(void *ctx, uint8_t reg, uint8_t val)
{
    chip_t *c = ctx;
    if (c->i2c_fail) return ESP_FAIL;
    const uint8_t old = c->r[reg];
    c->r[reg] = val;
    if (reg != MP2731_REG_ADC_CTRL) return ESP_OK;
    c->writes03++;
    const bool start = (val & MP2731_ADC_START_BIT) != 0;
    const bool edge = start && !(old & MP2731_ADC_START_BIT);
    if (!start || (c->edge_only && !edge)) return ESP_OK;
    if (c->adc_available) {
        c->conversions++;
        if (c->vary) {                       /* readings move like the bench */
            c->r[MP2731_REG_VSYS] = (uint8_t)(0xD7 + c->conversions % 6);
            c->r[MP2731_REG_IIN]  = (uint8_t)(0x08 + c->conversions % 9);
        }
    }
    if (c->latency_reads == 0) {
        c->r[reg] &= (uint8_t)~MP2731_ADC_START_BIT;
    } else {
        c->reads_left = c->latency_reads;       /* -1: START never falls */
    }
    return ESP_OK;
}

static void chip_delay(void *ctx, uint32_t ms) { ((chip_t *)ctx)->slept_ms += ms; }

/* Bench 2026-11-13 (28:37:2F:FF:E7:04 on USB): REG03=0x10, REG0C=0x39,
 * REG0D=0x80, results 0E=D1 0F=DA 11=55 12=00 13=0D. */
static void chip_bench(chip_t *c)
{
    memset(c, 0, sizeof *c);
    c->r[0x03] = 0x10; c->r[0x0C] = 0x39; c->r[0x0D] = 0x80;
    c->r[0x0E] = 0xD1; c->r[0x0F] = 0xDA; c->r[0x11] = 0x55; c->r[0x12] = 0x00; c->r[0x13] = 0x0D;
    c->adc_available = true; c->vary = true; c->latency_reads = 1;
}

/* The chip's power-on result registers with no conversion since (A3:FC). */
static void chip_por_no_conversion(chip_t *c, uint8_t status, uint8_t fault)
{
    memset(c, 0, sizeof *c);
    c->r[0x03] = 0x10; c->r[0x0C] = status; c->r[0x0D] = fault;
    c->adc_available = false; c->latency_reads = 0;
}

/* 14:63:93:39:37:30's frozen frame: 4.00/4.02 V, VIN 4.98 V, Iin 532, Icc 420. */
static void chip_frozen_charging(chip_t *c, uint8_t status, uint8_t fault)
{
    memset(c, 0, sizeof *c);
    c->r[0x03] = 0x10; c->r[0x0C] = status; c->r[0x0D] = fault;
    c->r[0x0E] = 200; c->r[0x0F] = 201; c->r[0x11] = 83; c->r[0x12] = 24; c->r[0x13] = 40;
    c->adc_available = false; c->latency_reads = 0;
}

static mp2731_bus_t bus_of(chip_t *c)
{
    mp2731_bus_t b = { .read = chip_read, .write = chip_write, .delay_ms = chip_delay, .ctx = c };
    return b;
}

static esp_err_t rd(chip_t *c, power_reading_t *p, mp2731_frame_t *f)
{
    const mp2731_bus_t b = bus_of(c);
    return mp2731_core_read(&b, p, f);
}

/* ── Gate driver: one evaluation per 5 s, like device_commands.c ─────────── */

typedef struct { power_gate_t g; int64_t now; } gate_run_t;

static power_gate_step_t gate_eval(gate_run_t *run, chip_t *c)
{
    power_reading_t p;
    const power_read_reason_t r = power_read_reason_from_err(rd(c, &p, NULL));
    const power_read_reason_t verdict = r == POWER_READ_NO_INPUT ? POWER_READ_OK : r;
    const power_gate_step_t st = power_gate_step(&run->g, run->now, verdict,
                                                 r == POWER_READ_OK && p.input_present);
    run->now += 5000;
    return st;
}

#ifdef LEGACY_IMPL
esp_err_t legacy_read_power(const mp2731_bus_t *bus, power_reading_t *out);
bool legacy_publish_power_ok(const mp2731_bus_t *bus, int64_t now);
#endif

/* Run `seconds` of gate evaluations on chip c; returns the final gate state. */
static bool incident_gate(chip_t *c, int seconds, bool start_open)
{
#ifdef LEGACY_IMPL
    /* The v2.16.0 gate: open it first with a healthy frame when asked. */
    const mp2731_bus_t b = bus_of(c);
    int64_t now = 0;
    bool open = false;
    if (start_open) {
        chip_t h; chip_bench(&h);
        const mp2731_bus_t hb = bus_of(&h);
        for (; now <= 20000; now += 5000) open = legacy_publish_power_ok(&hb, now);
        CHECK(open, "legacy gate failed to open on a healthy charger");
    }
    for (int64_t end = now + (int64_t)seconds * 1000; now <= end; now += 5000) {
        open = legacy_publish_power_ok(&b, now);
    }
    return open;
#else
    gate_run_t run = {0};
    power_gate_init(&run.g);
    if (start_open) {
        chip_t h; chip_bench(&h);
        while (run.now <= 20000) (void)gate_eval(&run, &h);
        CHECK(run.g.open, "gate failed to open on a healthy charger");
    }
    const int64_t end = run.now + (int64_t)seconds * 1000;
    while (run.now <= end) (void)gate_eval(&run, c);
    return run.g.open;
#endif
}

/* ── Driver scenarios ────────────────────────────────────────────────────── */

static void s_healthy(void)
{
    chip_t c; chip_bench(&c);
    power_reading_t a, b; mp2731_frame_t f;
    CHECK(rd(&c, &a, &f) == ESP_OK, "healthy read");
    CHECK(f.converted && f.conv_ms == MP2731_ADC_POLL_MS, "conv %u", (unsigned)f.conv_ms);
    CHECK(rd(&c, &b, NULL) == ESP_OK, "second read");
    CHECK(a.system_mv != b.system_mv || a.input_ma != b.input_ma, "readings must move");
    CHECK(a.battery_mv == 4180 && a.input_mv == 5100 && a.input_present, "decode %u %u",
          a.battery_mv, a.input_mv);
    CHECK(a.charge_status == 3 && a.status_reg == 0x39 && a.fault_reg == 0x80, "status");
    CHECK(c.conversions == 2, "one conversion per read");
}

static void s_stuck_start_bit_needs_edge(void)
{
    /* START left high (interrupted conversion / chip quirk) on an edge-triggered
     * ADC: `ctrl | START` writes 1 over 1 and never converts. */
    chip_t c; chip_bench(&c);
    c.edge_only = true;
    c.r[0x03] = 0x90;
    power_reading_t p;
#ifdef LEGACY_IMPL
    const mp2731_bus_t b = bus_of(&c);
    (void)legacy_read_power(&b, &p);
#else
    CHECK(rd(&c, &p, NULL) == ESP_OK, "read after clearing the stuck bit");
#endif
    CHECK(c.conversions == 1, "a stuck START bit must still produce a conversion (got %d)",
          c.conversions);
}

static void s_never_completes(void)
{
    chip_t c; chip_bench(&c);
    c.latency_reads = -1;
    c.r[0x03] = 0x2D;                   /* OTG bits that must survive */
    power_reading_t p; mp2731_frame_t f;
    CHECK(rd(&c, &p, &f) == POWER_ERR_ADC_TIMEOUT, "timeout verdict");
    CHECK(!f.converted && f.conv_ms == MP2731_ADC_TIMEOUT_MS, "bounded poll %u", (unsigned)f.conv_ms);
    CHECK(c.slept_ms == MP2731_ADC_TIMEOUT_MS, "slept %u", (unsigned)c.slept_ms);
    CHECK(c.r[0x03] == 0x2D, "START left low, OTG bits kept: %02X", c.r[0x03]);
    CHECK(power_read_reason_from_err(POWER_ERR_ADC_TIMEOUT) == POWER_READ_ADC_TIMEOUT, "map");
}

static void s_continuous_mode_cleared_bits_preserved(void)
{
    chip_t c; chip_bench(&c);
    c.r[0x03] = 0x40 | 0x2D;            /* continuous + OTG settings */
    power_reading_t p;
    CHECK(rd(&c, &p, NULL) == ESP_OK, "read");
    CHECK(c.r[0x03] == 0x2D, "one-shot, OTG bits preserved: %02X", c.r[0x03]);
}

static void s_all_zero_is_implausible(void)
{
    chip_t c; chip_por_no_conversion(&c, 0x20 /* VIN_STAT 001 */, 0x00);
    power_reading_t p;
    CHECK(rd(&c, &p, NULL) == POWER_ERR_IMPLAUSIBLE, "all-zero with input");
    chip_por_no_conversion(&c, 0x00, 0x00);  /* whole register file zero */
    CHECK(rd(&c, &p, NULL) == POWER_ERR_IMPLAUSIBLE, "zero file is not 'no input'");
}

static void s_no_input_status(void)
{
    /* Battery-only: VIN_STAT 000, INPUT_FAULT, not charging. The ADC is off
     * (datasheet p.26) and the frozen frame even claims charging. */
    chip_t c; chip_frozen_charging(&c, 0x01 /* VSYS_STAT */, MP2731_FAULT_INPUT | 0x80);
    power_reading_t p;
    CHECK(rd(&c, &p, NULL) == POWER_ERR_NO_INPUT, "no_input verdict");
    CHECK(!p.input_present, "no input");
    CHECK(power_read_reason_from_err(POWER_ERR_NO_INPUT) == POWER_READ_NO_INPUT, "map");
    CHECK(strcmp(power_read_reason_name(POWER_READ_NO_INPUT), "no_input") == 0, "name");
    /* Any disagreement → not "no input". */
    CHECK(!mp2731_core_no_input(0x01, 0x00), "INPUT_FAULT required");
    CHECK(!mp2731_core_no_input(0x21, MP2731_FAULT_INPUT), "VIN_STAT 000 required");
    CHECK(!mp2731_core_no_input(0x11, MP2731_FAULT_INPUT), "not charging required");
}

static void s_stale_charging_frame(void)
{
    /* Input present per status, charge current per ADC, but CHG_STAT = 00. */
    chip_t c; chip_frozen_charging(&c, 0x21 /* VIN_STAT 001, VSYS_STAT */, 0x80);
    power_reading_t p;
    CHECK(rd(&c, &p, NULL) == POWER_ERR_IMPLAUSIBLE, "stale charging frame");
    CHECK(p.charge_ma == 420 && p.charge_status == 0, "decoded %u/%u", p.charge_ma, p.charge_status);
}

static void s_plausibility_limits(void)
{
    power_reading_t p = { .battery_mv = 0, .system_mv = 3740, .input_mv = 5100,
                          .input_ma = 106, .charge_status = 0 };
    CHECK(mp2731_core_plausible(&p), "VBAT 0 with an idle charger is the field norm");
    p.system_mv = 2480;
    CHECK(!mp2731_core_plausible(&p), "VSYS below brown-out");
    p.system_mv = 5020;
    CHECK(!mp2731_core_plausible(&p), "VSYS above regulation");
    p.system_mv = 4000; p.battery_mv = 4820;
    CHECK(!mp2731_core_plausible(&p), "VBAT above a single cell");
    p.battery_mv = 4000; p.charge_ma = 17; p.charge_status = 0;
    CHECK(mp2731_core_plausible(&p), "one ICHG LSB of offset is tolerated");
    p.charge_ma = 35;
    CHECK(!mp2731_core_plausible(&p), "charging per ADC but not per status");
    p.charge_status = 2;
    CHECK(mp2731_core_plausible(&p), "consistent charging frame");
}

static void s_i2c_error(void)
{
    chip_t c; chip_bench(&c);
    c.i2c_fail = true;
    power_reading_t p;
    const esp_err_t err = rd(&c, &p, NULL);
    CHECK(err != ESP_OK && power_read_reason_from_err(err) == POWER_READ_I2C_ERROR, "bus error");
}

/* ── Gate policy scenarios ───────────────────────────────────────────────── */

static void s_gate_valid_unchanged(void)
{
    /* Healthy charger on external power: opens on the 15 s dwell, like v2.16.0. */
    gate_run_t run = {0}; power_gate_init(&run.g);
    chip_t c; chip_bench(&c);
    int64_t opened_at = -1;
    for (int i = 0; i < 6; i++) {
        const int64_t t = run.now;
        if (gate_eval(&run, &c) == POWER_GATE_STEP_OPENED) opened_at = t;
    }
    CHECK(opened_at == 15000, "opened at %lld", (long long)opened_at);
    CHECK(power_gate_flash_ok(&run.g), "valid external power allows flashing");
    CHECK(strcmp(power_gate_reason_name(run.g.reason), "external_power") == 0, "reason");
    /* Then valid "on battery" (dusk float VIN 4.26 V): closes after the 60 s dwell. */
    c.r[0x11] = 71; c.vary = false;
    int64_t closed_at = -1; const int64_t t0 = run.now;
    for (int i = 0; i < 16; i++) {
        const int64_t t = run.now;
        if (gate_eval(&run, &c) == POWER_GATE_STEP_CLOSED) closed_at = t - t0;
    }
    CHECK(closed_at == 60000, "closed after %lld ms", (long long)closed_at);
    CHECK(strcmp(power_gate_reason_name(run.g.reason), "on_battery") == 0, "reason");
}

static void s_gate_invalid_never_closes(void)
{
    gate_run_t run = {0}; power_gate_init(&run.g);
    for (int i = 0; i < 4; i++) power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, true);
    CHECK(run.g.open, "open");
    for (int i = 0; i < 10000; i++) {
        const power_read_reason_t r = (power_read_reason_t)(POWER_READ_ADC_TIMEOUT + i % 3);
        CHECK(power_gate_step(&run.g, run.now += 5000, r, false) == POWER_GATE_STEP_NONE, "step");
        CHECK(run.g.open, "invalid sample %d closed the gate", i);
    }
    CHECK(!power_gate_flash_ok(&run.g), "no flashing on an unreadable charger");
}

static void s_gate_failopen_after_n(void)
{
    gate_run_t run = {0}; power_gate_init(&run.g);
    for (unsigned i = 1; i < POWER_POLICY_INVALID_FAILOPEN_N; i++) {
        CHECK(power_gate_step(&run.g, run.now += 5000, POWER_READ_IMPLAUSIBLE, false) ==
              POWER_GATE_STEP_NONE && !run.g.open, "closed before N (%u)", i);
    }
    CHECK(power_gate_step(&run.g, run.now += 5000, POWER_READ_IMPLAUSIBLE, false) ==
          POWER_GATE_STEP_FAILOPEN && run.g.open, "fail-open at N");
    CHECK(strcmp(power_gate_reason_name(run.g.reason), "power_unverified") == 0, "reason");
    CHECK(!power_gate_flash_ok(&run.g), "AMBIT flash stays refused on fail-open");

    /* A valid present reading keeps it open; flashing needs the ON dwell. */
    power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, true);
    CHECK(!power_gate_flash_ok(&run.g), "one good frame is not evidence yet");
    power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, true);
    power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, true);
    power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, true);
    CHECK(power_gate_flash_ok(&run.g) && run.g.reason == POWER_GATE_REASON_EXTERNAL_POWER,
          "verified after the ON dwell");
}

static void s_gate_failopen_then_valid_absent_closes(void)
{
    gate_run_t run = {0}; power_gate_init(&run.g);
    for (unsigned i = 0; i < POWER_POLICY_INVALID_FAILOPEN_N; i++)
        power_gate_step(&run.g, run.now += 5000, POWER_READ_I2C_ERROR, false);
    CHECK(run.g.open, "fail-open");
    int closes = 0; const int64_t t0 = run.now;
    int64_t at = -1;
    for (int i = 0; i < 20; i++) {
        if (power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, false) == POWER_GATE_STEP_CLOSED) {
            closes++; at = run.now - t0;
        }
    }
    CHECK(closes == 1 && at == 65000, "valid absent closes on the normal dwell (%lld)", (long long)at);
}

static void s_gate_invalid_cancels_pending_close(void)
{
    gate_run_t run = {0}; power_gate_init(&run.g);
    for (int i = 0; i < 4; i++) power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, true);
    for (int i = 0; i < 11; i++) power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, false);
    CHECK(run.g.open, "50 s of absence: still open");
    power_gate_step(&run.g, run.now += 5000, POWER_READ_ADC_TIMEOUT, false);
    /* Without the cancel, the next valid absent sample (>60 s after the first)
     * would close at once. */
    CHECK(power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, false) == POWER_GATE_STEP_NONE &&
          run.g.open, "an invalid sample must not let the close mature");
    for (int i = 0; i < 12; i++) power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, false);
    CHECK(!run.g.open, "a fresh full OFF dwell still closes it");
}

static void s_gate_invalid_does_not_block_open(void)
{
    gate_run_t run = {0}; power_gate_init(&run.g);
    power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, true);
    power_gate_step(&run.g, run.now += 5000, POWER_READ_ADC_TIMEOUT, false);
    power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, true);
    CHECK(power_gate_step(&run.g, run.now += 5000, POWER_READ_OK, true) == POWER_GATE_STEP_OPENED,
          "an invalid sample does not restart the ON dwell");
}

static void s_gate_no_input_closes_like_battery(void)
{
    gate_run_t run = {0}; power_gate_init(&run.g);
    chip_t h; chip_bench(&h);
    while (run.now <= 20000) (void)gate_eval(&run, &h);
    CHECK(run.g.open, "open");
    chip_t c; chip_frozen_charging(&c, 0x01, MP2731_FAULT_INPUT);
    for (int i = 0; i < 14; i++) (void)gate_eval(&run, &c);
    CHECK(!run.g.open && run.g.reason == POWER_GATE_REASON_ON_BATTERY, "no_input closes");
    CHECK(run.g.invalid_streak == 0, "no_input is evidence, never counted toward fail-open");
}

/* ── SD park guard ───────────────────────────────────────────────────────── */

static void s_guard(void)
{
    power_guard_t g; power_guard_init(&g);
    const power_reading_t low = { .battery_mv = 3200, .system_mv = 3200 };
    const power_reading_t idle0 = { .battery_mv = 0, .system_mv = 3700 };
    const power_reading_t ext = { .battery_mv = 0, .system_mv = 4200, .input_present = true };

    for (int i = 0; i < 100; i++)
        CHECK(power_guard_step(&g, POWER_READ_IMPLAUSIBLE, &low, false) == POWER_GUARD_HOLD,
              "invalid never parks");
    for (int i = 0; i < 100; i++)
        CHECK(power_guard_step(&g, POWER_READ_OK, &idle0, false) == POWER_GUARD_HOLD,
              "VBAT 0 without input is no evidence");
    CHECK(power_guard_step(&g, POWER_READ_OK, &low, false) == POWER_GUARD_HOLD, "1");
    CHECK(power_guard_step(&g, POWER_READ_ADC_TIMEOUT, &low, false) == POWER_GUARD_HOLD, "x");
    CHECK(power_guard_step(&g, POWER_READ_OK, &low, false) == POWER_GUARD_HOLD, "1'");
    CHECK(power_guard_step(&g, POWER_READ_OK, &low, false) == POWER_GUARD_HOLD, "2'");
    CHECK(power_guard_step(&g, POWER_READ_OK, &low, true) == POWER_GUARD_HOLD, "maintenance veto");
    for (int i = 0; i < 2; i++) power_guard_step(&g, POWER_READ_OK, &low, false);
    CHECK(power_guard_step(&g, POWER_READ_OK, &low, false) == POWER_GUARD_PARK && g.parked, "park");

    /* Parked: N reads without evidence remount (never stay parked on none). */
    for (unsigned i = 1; i < POWER_POLICY_INVALID_FAILOPEN_N; i++)
        CHECK(power_guard_step(&g, POWER_READ_NO_INPUT, &low, false) == POWER_GUARD_HOLD, "hold");
    CHECK(power_guard_step(&g, POWER_READ_NO_INPUT, &low, false) == POWER_GUARD_UNPARK_UNVERIFIED &&
          !g.parked, "unpark after N without evidence");

    /* Valid recovery: 4 good reads (as before). */
    for (int i = 0; i < 3; i++) power_guard_step(&g, POWER_READ_OK, &low, false);
    CHECK(g.parked, "re-parked");
    for (int i = 0; i < 3; i++)
        CHECK(power_guard_step(&g, POWER_READ_OK, &ext, false) == POWER_GUARD_HOLD, "good %d", i);
    CHECK(power_guard_step(&g, POWER_READ_OK, &ext, false) == POWER_GUARD_UNPARK, "unpark");
}

/* ── Incident replays (red on v2.16.0, green now) ────────────────────────── */

static void s_incident_allzero(void)
{
    /* 14:63:93:37:A3:FC, 2026-11-13 01:13 UTC: the chip has restarted, its ADC
     * has not converted since (result registers at their 0x00 power-on value),
     * and the status is NOT a consistent "no input". v2.16.0 read "no input"
     * from the zeros and held the gate closed for 9+ h (2,519 records). */
    chip_t c; chip_por_no_conversion(&c, 0x20, 0x00);
    const bool open = incident_gate(&c, 10 * 60, false);
    CHECK(open, "an all-zero charger frame held the publish gate CLOSED for 10 min "
                "(field data held back)");
}

static void s_incident_stale_open_on_battery(void)
{
    /* 14:63:93:39:37:30: a CHARGING frame frozen through the night (×181) with the
     * live status saying no input. v2.16.0 believed the frame (VIN 4.98 V) and
     * kept publishing on a dying battery. */
    chip_t c; chip_frozen_charging(&c, 0x01, MP2731_FAULT_INPUT);
    const bool open = incident_gate(&c, 10 * 60, true);
    CHECK(!open, "a frozen charging frame kept the publish gate OPEN on battery");
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*fn)(void); } S[] = {
        {"healthy", s_healthy},
        {"stuck_start_bit_needs_edge", s_stuck_start_bit_needs_edge},
        {"never_completes", s_never_completes},
        {"continuous_mode_cleared_bits_preserved", s_continuous_mode_cleared_bits_preserved},
        {"all_zero_is_implausible", s_all_zero_is_implausible},
        {"no_input_status", s_no_input_status},
        {"stale_charging_frame", s_stale_charging_frame},
        {"plausibility_limits", s_plausibility_limits},
        {"i2c_error", s_i2c_error},
        {"gate_valid_unchanged", s_gate_valid_unchanged},
        {"gate_invalid_never_closes", s_gate_invalid_never_closes},
        {"gate_failopen_after_n", s_gate_failopen_after_n},
        {"gate_failopen_then_valid_absent_closes", s_gate_failopen_then_valid_absent_closes},
        {"gate_invalid_cancels_pending_close", s_gate_invalid_cancels_pending_close},
        {"gate_invalid_does_not_block_open", s_gate_invalid_does_not_block_open},
        {"gate_no_input_closes_like_battery", s_gate_no_input_closes_like_battery},
        {"guard", s_guard},
        {"incident_allzero", s_incident_allzero},
        {"incident_stale_open_on_battery", s_incident_stale_open_on_battery},
    };
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) printf("%s\n", S[i].name);
        return 0;
    }
    for (size_t i = 0; argc == 2 && i < sizeof S / sizeof S[0]; i++) {
        if (strcmp(argv[1], S[i].name) == 0) {
            S[i].fn();
            printf("PASS %s\n", S[i].name);
            return 0;
        }
    }
    fprintf(stderr, "usage: %s <scenario>|--list\n", argv[0]);
    return 2;
}
