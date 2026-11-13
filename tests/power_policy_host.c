/* Host harness: the production MP2731 read (components/mp2731/mp2731_core.c) and
 * power policy (components/device_commands/power_policy.c) driven by a fake
 * charger behind the register seam. tests/test_power_policy.py runs every
 * scenario; built with -DLEGACY_IMPL the incident scenarios run the v2.16.0
 * code instead (tests/fixtures/power_v2_16_0) and must FAIL - the red half of
 * the red/green reproduction of the 2026-11-13 Uluru incident.
 *
 * The incident replays are MODELED, not captured: field heartbeats carry the
 * ADC frame and CHG_STAT only, never REG0C/REG0D in full. The all-zero replay
 * ASSUMES live status 0x20 (VIN_STAT 001) with fault 0x00; the frozen-frame
 * replay ASSUMES status 0x01 with INPUT_FAULT set. Assumptions consistent with
 * the datasheet, not field captures.
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

/* ── Policy driver: one charger read per 5 s, like device_commands.c ─────── */

static power_read_reason_t eval(power_gate_t *g, chip_t *c)
{
    power_reading_t p;
    const power_read_reason_t r = power_read_reason_from_err(rd(c, &p, NULL));
    (void)power_gate_step(g, r, r == POWER_READ_OK ? &p : NULL);
    return r;
}

/* Feed a synthetic valid frame; `seq` varies the LSBs like a converting ADC. */
static void feed_ok(power_gate_t *g, bool present, int seq)
{
    power_reading_t p = { .battery_mv = (uint16_t)(4180 + 20 * (seq % 2)),
                          .system_mv = (uint16_t)(4300 + 20 * (seq % 3)),
                          .input_mv = present ? 5100 : 0,
                          .input_ma = (uint16_t)(106 + 13 * (seq % 4)),
                          .input_present = present, .charge_status = present ? 3 : 0 };
    (void)power_gate_step(g, POWER_READ_OK, &p);
}

#ifdef LEGACY_IMPL
esp_err_t legacy_read_power(const mp2731_bus_t *bus, power_reading_t *out);
bool legacy_publish_power_ok(const mp2731_bus_t *bus, int64_t now);
#endif

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

/* ── Publish gate: never closes ──────────────────────────────────────────── */

static void s_publish_never_closes(void)
{
    /* Every reading class, interleaved, for ~28 h of 5 s evaluations. */
    power_gate_t g; power_gate_init(&g);
    CHECK(power_publish_ok(&g), "open from boot");
    static const power_read_reason_t R[] = {
        POWER_READ_OK, POWER_READ_NO_INPUT, POWER_READ_ADC_TIMEOUT,
        POWER_READ_IMPLAUSIBLE, POWER_READ_I2C_ERROR, POWER_READ_UNAVAILABLE };
    for (int i = 0; i < 20000; i++) {
        const power_read_reason_t r = R[(i * 7 + i / 13) % 6];
        if (r == POWER_READ_OK) feed_ok(&g, (i / 3) % 2 == 0, i);
        else (void)power_gate_step(&g, r, NULL);
        CHECK(power_publish_ok(&g), "closed at step %d (reason %s)", i, power_read_reason_name(r));
    }
    /* A long run of fresh, valid "absent" frames: still open. */
    for (int i = 0; i < 1000; i++) {
        feed_ok(&g, false, i);
        CHECK(power_publish_ok(&g), "valid absent closed it");
    }
    CHECK(g.reason == POWER_GATE_REASON_ON_BATTERY, "reported on battery");
}

static void s_power_state_reason(void)
{
    power_gate_t g; power_gate_init(&g);
    CHECK(strcmp(power_gate_reason_name(g.reason), "starting") == 0, "starting");
    feed_ok(&g, true, 0);
    CHECK(strcmp(power_gate_reason_name(g.reason), "external_power") == 0, "external");
    CHECK(power_gate_step(&g, POWER_READ_NO_INPUT, NULL) && g.reason == POWER_GATE_REASON_ON_BATTERY,
          "no_input reports on_battery");
    (void)power_gate_step(&g, POWER_READ_ADC_TIMEOUT, NULL);
    CHECK(strcmp(power_gate_reason_name(g.reason), "power_unverified") == 0, "unverified");
    CHECK(g.invalid_streak == 1, "streak");
    (void)power_gate_step(&g, POWER_READ_NO_INPUT, NULL);
    CHECK(g.invalid_streak == 0, "no_input is evidence, not an unreadable charger");
    CHECK(strcmp(power_gate_reason_name(POWER_GATE_REASON_NO_CHARGER), "no_charger") == 0, "name");
}

static void s_eu_night(void)
{
    /* Healthy solar unit: day on the panel, dusk (valid frames, VIN 4.26 V below
     * the 4.7 V source threshold), night (ADC off, live status "no input"). It
     * keeps publishing throughout - the battery cost accepted on 2026-11-13 -
     * and may not flash an AMBIT after dusk. */
    power_gate_t g; power_gate_init(&g);
    chip_t day; chip_bench(&day);
    for (int i = 0; i < 6; i++) (void)eval(&g, &day);
    CHECK(g.reason == POWER_GATE_REASON_EXTERNAL_POWER && power_gate_flash_ok(&g), "day");
    chip_t dusk; chip_bench(&dusk);
    dusk.r[0x11] = 71; dusk.r[0x0C] = 0x21;              /* VIN 4.26 V, not charging */
    for (int i = 0; i < 20; i++) {
        CHECK(eval(&g, &dusk) == POWER_READ_OK, "dusk frames are valid");
        CHECK(power_publish_ok(&g) && !power_gate_flash_ok(&g), "dusk: publish, no flash");
    }
    CHECK(g.reason == POWER_GATE_REASON_ON_BATTERY, "dusk reported on battery");
    chip_t night; chip_frozen_charging(&night, 0x01, MP2731_FAULT_INPUT);
    for (int i = 0; i < 720; i++) {
        CHECK(eval(&g, &night) == POWER_READ_NO_INPUT, "night");
        CHECK(power_publish_ok(&g) && !power_gate_flash_ok(&g), "night: publish, no flash");
    }
}

/* ── AMBIT flash gate: its own fresh, positive evidence ─────────────────── */

static void s_flash_needs_fresh_present(void)
{
    power_gate_t g; power_gate_init(&g);
    feed_ok(&g, true, 0);
    CHECK(!power_gate_flash_ok(&g), "one frame is not freshness evidence");
    feed_ok(&g, true, 1);
    CHECK(power_gate_flash_ok(&g), "two differing valid present frames");

    /* Codex B1 (a): input lost where the old publish OFF dwell used to run. */
    power_gate_init(&g);
    for (int i = 0; i < 4; i++) feed_ok(&g, true, i);          /* 0/5/10/15 s */
    CHECK(power_gate_flash_ok(&g), "verified");
    feed_ok(&g, false, 4);                                      /* 20 s */
    CHECK(!power_gate_flash_ok(&g), "refused at once on a valid absent frame");
    feed_ok(&g, true, 5);
    CHECK(power_gate_flash_ok(&g), "present again with a changed frame");
    (void)power_gate_step(&g, POWER_READ_NO_INPUT, NULL);
    CHECK(!power_gate_flash_ok(&g), "refused at once on no_input");

    /* Codex B1 (b): an interrupted dwell - invalid samples are not evidence. */
    power_gate_init(&g);
    feed_ok(&g, true, 0);                                       /* 0 s  */
    (void)power_gate_step(&g, POWER_READ_ADC_TIMEOUT, NULL);    /* 5 s  */
    (void)power_gate_step(&g, POWER_READ_IMPLAUSIBLE, NULL);    /* 10 s */
    feed_ok(&g, true, 1);                                       /* 15 s */
    CHECK(!power_gate_flash_ok(&g), "invalid samples break the evidence chain");
    feed_ok(&g, true, 2);
    CHECK(power_gate_flash_ok(&g), "two fresh frames after the gap");

    for (int r = POWER_READ_ADC_TIMEOUT; r <= POWER_READ_NO_INPUT; r++) {
        power_gate_init(&g);
        feed_ok(&g, true, 0); feed_ok(&g, true, 1);
        (void)power_gate_step(&g, (power_read_reason_t)r, NULL);
        CHECK(!power_gate_flash_ok(&g), "refused on %s",
              power_read_reason_name((power_read_reason_t)r));
    }
}

static void s_flash_refuses_frozen_frame(void)
{
    /* A frozen but internally consistent charging frame (status CC charging,
     * input present): the read says ok, the frame never changes. */
    power_gate_t g; power_gate_init(&g);
    chip_t c; chip_frozen_charging(&c, 0x31 /* VIN_STAT 001, CHG CC */, 0x80);
    for (int i = 0; i < 50; i++) {
        CHECK(eval(&g, &c) == POWER_READ_OK, "consistent frame reads ok");
        CHECK(c.conversions == 0 && power_publish_ok(&g), "no conversion, still publishing");
        CHECK(!power_gate_flash_ok(&g), "an unchanging frame is never flash evidence (%d)", i);
    }
    /* Changing frames that then freeze: the evidence expires after the window. */
    chip_t h; chip_bench(&h);
    for (int i = 0; i < 4; i++) (void)eval(&g, &h);
    CHECK(power_gate_flash_ok(&g), "live ADC");
    h.adc_available = false;
    for (unsigned i = 0; i < POWER_FLASH_FRESH_WINDOW + 1; i++) (void)eval(&g, &h);
    CHECK(!power_gate_flash_ok(&g), "frozen for the window: refused");
}

static void s_twilight_stale_frame(void)
{
    /* Codex L3: no conversion, a plausible twilight frame, live status that is
     * not "no input" (0x01 / fault 0x00). Indistinguishable from fresh: the read
     * says ok. Publishing continues; flashing and parking refuse. */
    chip_t c; memset(&c, 0, sizeof c);
    c.r[0x03] = 0x10; c.r[0x0C] = 0x01; c.r[0x0D] = 0x00;
    c.r[0x0E] = 200; c.r[0x0F] = 201; c.r[0x11] = 71; c.r[0x12] = 0; c.r[0x13] = 0;
    power_gate_t g; power_gate_init(&g);
    power_guard_t guard; power_guard_init(&guard);
    for (int i = 0; i < 100; i++) {
        power_reading_t p;
        const power_read_reason_t r = power_read_reason_from_err(rd(&c, &p, NULL));
        CHECK(r == POWER_READ_OK && c.conversions == 0, "stale twilight frame reads ok");
        (void)power_gate_step(&g, r, &p);
        CHECK(power_publish_ok(&g) && !power_gate_flash_ok(&g), "publish yes, flash no");
        CHECK(power_guard_step(&guard, r, &p, false) == POWER_GUARD_HOLD, "never parks");
    }
}

/* ── SD park guard: fresh, changing evidence only ───────────────────────── */

static power_reading_t low_frame(int seq)
{
    power_reading_t p = { .battery_mv = (uint16_t)(3200 - 20 * (seq % 3)),
                          .system_mv = (uint16_t)(3200 - 20 * (seq % 3)),
                          .input_ma = (uint16_t)(13 * (seq % 2)) };
    return p;
}

static void s_guard(void)
{
    power_guard_t g; power_guard_init(&g);
    const power_reading_t low = low_frame(0);

    for (int i = 0; i < 100; i++)
        CHECK(power_guard_step(&g, POWER_READ_IMPLAUSIBLE, &low, false) == POWER_GUARD_HOLD,
              "invalid never parks");
    for (int i = 0; i < 100; i++)
        CHECK(power_guard_step(&g, POWER_READ_NO_INPUT, &low, false) == POWER_GUARD_HOLD,
              "no_input never parks");
    for (int i = 0; i < 100; i++)
        CHECK(power_guard_step(&g, POWER_READ_OK, &low, false) == POWER_GUARD_HOLD,
              "an unchanging low frame never parks");
    for (int i = 0; i < 100; i++) {
        power_reading_t p = { .battery_mv = 0, .system_mv = (uint16_t)(3700 + 20 * (i % 2)) };
        CHECK(power_guard_step(&g, POWER_READ_OK, &p, false) == POWER_GUARD_HOLD,
              "VBAT 0 without input is no evidence");
    }

    /* Fresh, changing low frames: park after PARK_N; maintenance vetoes. */
    power_guard_init(&g);
    int seq = 0;
    power_reading_t f = low_frame(seq++);
    CHECK(power_guard_step(&g, POWER_READ_OK, &f, false) == POWER_GUARD_HOLD, "first frame: no prev");
    f = low_frame(seq++); CHECK(power_guard_step(&g, POWER_READ_OK, &f, false) == POWER_GUARD_HOLD, "1");
    f = low_frame(seq++); CHECK(power_guard_step(&g, POWER_READ_OK, &f, false) == POWER_GUARD_HOLD, "2");
    f = low_frame(seq++); CHECK(power_guard_step(&g, POWER_READ_OK, &f, true) == POWER_GUARD_HOLD, "veto");
    for (int i = 0; i < 2; i++) { f = low_frame(seq++); (void)power_guard_step(&g, POWER_READ_OK, &f, false); }
    f = low_frame(seq++);
    CHECK(power_guard_step(&g, POWER_READ_OK, &f, false) == POWER_GUARD_PARK && g.parked, "park");

    /* Parked: N reads without evidence remount (never stay parked on none). */
    for (unsigned i = 1; i < POWER_POLICY_INVALID_FAILOPEN_N; i++)
        CHECK(power_guard_step(&g, POWER_READ_NO_INPUT, &low, false) == POWER_GUARD_HOLD, "hold");
    CHECK(power_guard_step(&g, POWER_READ_NO_INPUT, &low, false) == POWER_GUARD_UNPARK_UNVERIFIED &&
          !g.parked, "unpark after N without evidence");

    /* Re-park, then valid recovery: 4 fresh good reads (as before). */
    for (int i = 0; i < 4; i++) { f = low_frame(seq++); (void)power_guard_step(&g, POWER_READ_OK, &f, false); }
    CHECK(g.parked, "re-parked");
    for (int i = 0; i < 3; i++) {
        power_reading_t e = { .battery_mv = 0, .system_mv = (uint16_t)(4200 + 20 * (i % 2)),
                              .input_mv = 5100, .input_present = true };
        CHECK(power_guard_step(&g, POWER_READ_OK, &e, false) == POWER_GUARD_HOLD, "good %d", i);
    }
    power_reading_t e = { .battery_mv = 0, .system_mv = 4240, .input_mv = 5100, .input_present = true };
    CHECK(power_guard_step(&g, POWER_READ_OK, &e, false) == POWER_GUARD_UNPARK, "unpark");
}

/* ── Incident replays (MODELED; red on v2.16.0, green now) ──────────────── */

static void s_incident_allzero(void)
{
    /* 14:63:93:37:A3:FC, 2026-11-13 from 01:13 UTC: all-zero frame for 9+ h.
     * MODELED live registers (assumed, not captured): status 0x20, fault 0x00.
     * v2.16.0 read "no input" from the zeros and held the gate closed. */
    chip_t c; chip_por_no_conversion(&c, 0x20, 0x00);
    bool open = false;
#ifdef LEGACY_IMPL
    const mp2731_bus_t b = bus_of(&c);
    for (int64_t now = 0; now <= 10 * 60 * 1000; now += 5000) open = legacy_publish_power_ok(&b, now);
#else
    power_gate_t g; power_gate_init(&g);
    for (int i = 0; i <= 120; i++) {
        (void)eval(&g, &c);
        open = power_publish_ok(&g);
        if (!open) break;
    }
#endif
    CHECK(open, "an all-zero charger frame held the publish gate CLOSED "
                "(field data held back)");
}

static void s_incident_frozen_frame_flash(void)
{
    /* 14:63:93:39:37:30: one CHARGING frame (charge 420 mA, VIN 4.98 V) frozen
     * for 10+ h while the heartbeat's CHG_STAT said "not charging". MODELED live
     * registers (assumed, not captured): status 0x01, INPUT_FAULT set. v2.16.0
     * believed the frame; its AMBIT boot-sync flash gate (the publish gate
     * itself) opened and would have flashed without external power. */
    chip_t c; chip_frozen_charging(&c, 0x01, MP2731_FAULT_INPUT);
    bool flash = false;
#ifdef LEGACY_IMPL
    const mp2731_bus_t b = bus_of(&c);
    for (int64_t now = 0; now <= 10 * 60 * 1000; now += 5000) flash = legacy_publish_power_ok(&b, now);
#else
    power_gate_t g; power_gate_init(&g);
    for (int i = 0; i <= 120; i++) { (void)eval(&g, &c); flash = flash || power_gate_flash_ok(&g); }
#endif
    CHECK(!flash, "a frozen charging frame let an AMBIT flash proceed without external power");
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
        {"publish_never_closes", s_publish_never_closes},
        {"power_state_reason", s_power_state_reason},
        {"eu_night", s_eu_night},
        {"flash_needs_fresh_present", s_flash_needs_fresh_present},
        {"flash_refuses_frozen_frame", s_flash_refuses_frozen_frame},
        {"twilight_stale_frame", s_twilight_stale_frame},
        {"guard", s_guard},
        {"incident_allzero", s_incident_allzero},
        {"incident_frozen_frame_flash", s_incident_frozen_frame_flash},
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
