/* Maintenance vs charger-mutation admission (maint_gate.h), against the
 * production MP2731 adapter (this translation unit) and a fake charger.
 * The gate is the real header the composition root uses; the test plays the
 * maintenance worker by calling maint_gate_request/poll/abandon/end at chosen
 * moments. Every mutation: VIN set, IIN set, AICO on/off/reset/automatic
 * re-arm, manual BG_EN and DP/DM re-detect, automatic re-detect pulse.
 *   - maintenance requested at EACH I/O step of each mutation -> it waits
 *     (never granted mid-mutation), the mutation completes, and maintenance is
 *     granted right after; meanwhile a new mutation is refused (no starvation);
 *   - maintenance held -> commands return ESP_ERR_NOT_ALLOWED with no bus
 *     access at all, automatic actions write nothing, and run after it ends;
 *   - maintenance beginning right AFTER a veto probe (the Codex P1 race) ->
 *     the admission refuses, nothing is written;
 *   - an abandoned request re-admits mutations.
 * The write fake asserts that no charger register is written while
 * maintenance is granted, and none outside an admitted mutation.
 * usage: charger_gate_host <case> (exit 0 = pass) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/semphr.h"

#include "../components/mp2731/mp2731.c"
#include "maint_gate.h"

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

#define S 1000LL

/* ── Gate + maintenance worker model ──────────────────────────────────── */
static maint_gate_t g;
static int in_mutation, ops_in_window, inject_at = -1, windows;
static int injected = -1;
static int race_after_probe;          /* the next veto probe is followed by a maintenance grant */

static bool gate_begin(void)
{
    const bool ok = maint_gate_mutation_begin(&g);
    if (ok) { in_mutation++; ops_in_window = 0; windows++; }
    return ok;
}
static void gate_end(void) { maint_gate_mutation_end(&g); in_mutation--; assert(in_mutation >= 0); }
static bool veto(void)
{
    const bool active = maint_gate_active(&g);
    if (race_after_probe) {           /* maintenance begins between probe and admission */
        race_after_probe = 0;
        CHECK(maint_gate_request(&g) == MAINT_GATE_GRANTED, "race grant");
    }
    return active;
}
static void io_step(void)
{
    if (!in_mutation) return;
    ops_in_window++;
    if (ops_in_window == inject_at) {
        inject_at = -1;               /* one request, like one maintenance worker */
        injected = maint_gate_request(&g);
        CHECK(injected == MAINT_GATE_WAIT, "maintenance requested mid-mutation must wait (%d)", injected);
    }
}

/* ── Fake charger ─────────────────────────────────────────────────────── */
static uint8_t regs[24];
static int64_t clock_us = 1000000;
static int mutex_held, locked, sessions, writes, bus_ops;
static bool dpdm_keep;
static unsigned conversions;
static int takes, take_fail_at = -1;  /* HAR-310: the Nth read-mutex take times out (one-shot) */
/* PR #112 review: REG0B writes allowed before the bus starts failing them
 * (-1 = never fail), and BG_EN clears the chip ignores while > 0. */
static int ok_0b_writes = -1, bg_stuck;
static int bg_writes_busy, dpdm_writes_busy;  /* REG09/REG0B written while maintenance runs */
static int fail_reads_09;                     /* PR #112 re-review: REG09 reads that fail */

esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
void h_critical_enter(portMUX_TYPE *m) { assert(!m->held); m->held = 1; }
void h_critical_exit(portMUX_TYPE *m) { assert(m->held); m->held = 0; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_held; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{
    (void)wait; assert(m == &mutex_held && !mutex_held);
    if (++takes == take_fail_at) { take_fail_at = -1; return pdFALSE; }
    mutex_held = 1; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { assert(m == &mutex_held && mutex_held); mutex_held = 0; return pdTRUE; }
int64_t esp_timer_get_time(void) { return clock_us; }
void esp_rom_delay_us(unsigned us) { clock_us += us; }
void vTaskDelay(TickType_t ticks) { assert(ticks < 100 || !mutex_held); clock_us += (int64_t)ticks * 1000; }
const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "error"; }
esp_err_t i2c_bus_get_port(i2c_port_t *p) { *p = 0; return ESP_OK; }
esp_err_t i2c_bus_lock(unsigned timeout) { assert(timeout > 0 && !locked); locked = 1; sessions++; return ESP_OK; }
esp_err_t i2c_bus_unlock(void) { assert(locked); locked = 0; return ESP_OK; }
esp_err_t i2c_bus_probe_addr(i2c_port_t p, unsigned a) { (void)p; assert(a == 0x4B); return ESP_OK; }
esp_err_t i2c_master_write_read_device(int port, unsigned addr, const uint8_t *w, size_t wn,
                                       uint8_t *r, size_t rn, unsigned timeout)
{
    (void)port; (void)timeout;
    assert(locked && addr == 0x4B && wn == 1 && rn == 1 && *w < 24);
    io_step(); bus_ops++;
    if (*w == 0x09 && fail_reads_09 > 0) { fail_reads_09--; return ESP_FAIL; }
    *r = regs[*w];
    return ESP_OK;
}
esp_err_t i2c_master_write_to_device(int port, unsigned addr, const uint8_t *w, size_t wn, unsigned timeout)
{
    (void)port; (void)timeout;
    assert(locked && addr == 0x4B && wn == 2);
    io_step(); bus_ops++;
    const uint8_t v = w[1];
    if (w[0] != 0x03) {
        /* Every charger configuration write: inside an admitted mutation, and
         * never while maintenance runs. (REG03 is ADC control, part of reads.) */
        CHECK(in_mutation > 0, "REG%02X written outside an admitted mutation", w[0]);
        CHECK(!g.busy, "REG%02X written while maintenance runs", w[0]);
        writes++;
    }
    switch (w[0]) {
    case 0x03:
        regs[3] = v & 0x7F;
        if (v & 0x80) { conversions++; regs[0x11] = (uint8_t)(124 + conversions % 2); regs[0x0F] = (uint8_t)(175 + conversions % 2); }
        return ESP_OK;
    case 0x0B:
        if (g.busy) dpdm_writes_busy++;
        if (ok_0b_writes == 0) return ESP_FAIL;
        if (ok_0b_writes > 0) ok_0b_writes--;
        regs[0x0B] = dpdm_keep ? v : (uint8_t)(v & ~0x20);
        return ESP_OK;
    case 0x09:
        if (g.busy) bg_writes_busy++;
        if ((regs[9] & 0x08) && !(v & 0x08) && bg_stuck > 0) { bg_stuck--; return ESP_OK; }
        regs[9] = v;
        return ESP_OK;
    default: regs[w[0]] = v; return ESP_OK;
    }
}

static void field_state(void)
{
    static const uint8_t field[24] = {
        [0x00] = 0x48, [0x01] = 0x06, [0x02] = 0xDC, [0x03] = 0x10, [0x04] = 0x5B,
        [0x08] = 0x9D, [0x09] = 0x05, [0x0B] = 0xC6, [0x0C] = 0x00, [0x0D] = 0x80,
        [0x0F] = 175, [0x10] = 102, [0x11] = 125, [0x14] = 0x08, [0x15] = 0x60, [0x17] = 0x00,
    };
    memcpy(regs, field, sizeof regs);
}

static void reads_for(int64_t ms)
{
    for (int64_t end = clock_us + ms * 1000; clock_us < end; ) {
        const int64_t t = clock_us;
        power_reading_t p;
        (void)mp2731_read_power(&p);
        assert(!mutex_held && !locked);
        clock_us = t + 5000000;
    }
}

static void fresh(void)
{
    field_state();
    memset(&g, 0, sizeof g);
    in_mutation = 0; inject_at = -1; injected = -1; windows = 0; writes = 0; race_after_probe = 0;
    s_marker_checked = false; s_manual_active = false; s_bg_en_dirty = false; s_prev_adc_valid = false;
    s_redetect_marker = 0;
    assert(mp2731_init() == ESP_OK);
    mp2731_redetect_set_veto(veto);
    mp2731_set_mutation_gate(gate_begin, gate_end);
}

/* ── The mutations ────────────────────────────────────────────────────── */
static power_vin_limit_t vin; static power_iin_limit_t iin; static power_aico_t aico;
static power_redetect_report_t rep;
static esp_err_t m_vin(void) { return mp2731_set_vin_limit(4000, &vin); }
static esp_err_t m_iin(void) { return mp2731_set_iin_limit(1000, &iin); }
static esp_err_t m_aico_on(void) { regs[2] = 0xDC; return mp2731_set_aico(true, &aico); }
static esp_err_t m_aico_off(void) { regs[2] = 0xDD; return mp2731_set_aico(false, &aico); }
static esp_err_t m_aico_reset(void) { return mp2731_reset_aico(&aico); }
static esp_err_t m_bg_en(void) { return mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep); }
static esp_err_t m_dpdm(void) { dpdm_keep = true; return mp2731_charger_redetect(POWER_REDETECT_DPDM, &rep); }
/* Automatic: ESP_OK when the action ran (an admitted window with writes). */
static esp_err_t m_aico_auto(void)
{
    regs[2] = 0xDD;
    reads_for(5 * S);                 /* starts the policy clock if needed */
    s_aico.next_ms = uptime_ms();     /* due now */
    const int w = windows, before = writes;
    reads_for(5 * S);
    return windows > w && writes > before ? ESP_OK : ESP_ERR_NOT_ALLOWED;
}
static esp_err_t m_redetect_auto(void)
{
    const int w = windows, before = writes;
    for (int i = 0; i < 60 && windows == w; i++) reads_for(5 * S);
    return windows > w && writes > before ? ESP_OK : ESP_ERR_NOT_ALLOWED;
}

typedef struct { const char *name; esp_err_t (*run)(void); bool automatic; bool probes; } mutation_t;
static const mutation_t M[] = {
    {"vin_set", m_vin, false, false},          {"iin_set", m_iin, false, false},
    {"aico_on", m_aico_on, false, false},      {"aico_off", m_aico_off, false, false},
    {"aico_reset", m_aico_reset, false, true}, {"aico_auto", m_aico_auto, true, true},
    {"redetect_bg_en", m_bg_en, false, true},  {"redetect_dpdm", m_dpdm, false, true},
    {"redetect_auto", m_redetect_auto, true, true},
};

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *c = argv[1];
    if (!strcmp(c, "gate")) {
        /* The pure gate on its own. */
        maint_gate_t t = {0};
        CHECK(maint_gate_request(&t) == MAINT_GATE_GRANTED, "free: granted");
        CHECK(maint_gate_request(&t) == MAINT_GATE_BUSY && !maint_gate_mutation_begin(&t), "busy");
        maint_gate_end(&t);
        CHECK(maint_gate_mutation_begin(&t) && maint_gate_mutation_begin(&t), "mutations nest");
        CHECK(maint_gate_request(&t) == MAINT_GATE_WAIT && maint_gate_request(&t) == MAINT_GATE_BUSY,
              "one pending requester");
        CHECK(!maint_gate_mutation_begin(&t), "pending refuses");
        maint_gate_mutation_end(&t);
        CHECK(maint_gate_poll(&t) == MAINT_GATE_WAIT, "one still running");
        maint_gate_mutation_end(&t);
        CHECK(maint_gate_poll(&t) == MAINT_GATE_GRANTED && t.busy && !t.pending, "drained: granted");
        maint_gate_end(&t);
        maint_gate_mutation_end(&t);
        CHECK(t.mutations == 0, "end never underflows");
        CHECK(maint_gate_poll(&t) == MAINT_GATE_BUSY, "poll without a request");
        puts("OK");
        return 0;
    }
    if (!strcmp(c, "quarantine_dpdm") || !strcmp(c, "quarantine_dpdm_pending")) {
        /* PR #112 review P1: a DP/DM restore that fails must not let
         * maintenance be granted with REG0B bit 5 set, and its recovery must
         * drain a PENDING request, never write under a GRANTED one. */
        const bool pending = !strcmp(c, "quarantine_dpdm_pending");
        fresh();
        ok_0b_writes = 1;                       /* the force lands, the restore fails */
        if (pending) inject_at = 1;             /* maintenance requested mid-command */
        const esp_err_t r = m_dpdm();
        CHECK(r != ESP_OK && !rep.cleared && (regs[0x0B] & 0x20), "restore failed (%d)", r);
        if (!pending) {
            CHECK(maint_gate_request(&g) == MAINT_GATE_WAIT,
                  "maintenance granted with REG0B bit 5 asserted (0x%02X)", regs[0x0B]);
        }
        CHECK(maint_gate_poll(&g) == MAINT_GATE_WAIT && !g.busy,
              "maintenance granted with REG0B bit 5 asserted (0x%02X)", regs[0x0B]);
        CHECK(g.mutations == 1, "the failed cleanup must hold the gate (mutations %u)", g.mutations);
        /* Bus still failing: reads keep retrying, maintenance keeps waiting,
         * then gives up after its bound; the quarantine stays. */
        reads_for(20 * S);
        CHECK(maint_gate_poll(&g) == MAINT_GATE_WAIT && (regs[0x0B] & 0x20), "still not proven");
        maint_gate_abandon(&g);
        CHECK(g.mutations == 1 && maint_gate_request(&g) == MAINT_GATE_WAIT, "a new request waits too");
        /* Bus back: the next read restores while maintenance is pending. */
        ok_0b_writes = -1;
        reads_for(5 * S);
        CHECK(!(regs[0x0B] & 0x20) && g.mutations == 0, "recovery drained (0x%02X, %u)", regs[0x0B], g.mutations);
        CHECK(maint_gate_poll(&g) == MAINT_GATE_GRANTED, "granted only after verified recovery");
        reads_for(60 * S);
        CHECK(dpdm_writes_busy == 0 && bg_writes_busy == 0, "no REG09/REG0B write under granted maintenance");
        maint_gate_end(&g);
        CHECK(!locked && !mutex_held && in_mutation == 0 && g.mutations == 0, "clean exit");
        puts("OK");
        return 0;
    }
    if (!strcmp(c, "quarantine_bg_en") || !strcmp(c, "quarantine_bg_en_auto")) {
        /* The same for BG_EN, manual and automatic: its recovery is a 1->0
         * edge (the re-detect edge itself) and must never meet GRANTED
         * maintenance. Pulse: 1 + 3 clears ignored, then one more by the
         * first recovery read. */
        const bool automatic = !strcmp(c, "quarantine_bg_en_auto");
        fresh();
        bg_stuck = 1 + (int)MP2731_BG_EN_CLEAR_RETRIES + 1;
        if (automatic) {
            CHECK(m_redetect_auto() == ESP_OK && (regs[9] & 0x08), "auto pulse left BG_EN set");
        } else {
            CHECK(m_bg_en() == ESP_ERR_INVALID_STATE && (regs[9] & 0x08), "manual pulse left BG_EN set");
        }
        CHECK(maint_gate_request(&g) == MAINT_GATE_WAIT, "maintenance granted over a dirty BG_EN");
        CHECK(g.mutations == 1, "dirty BG_EN must hold the gate (mutations %u)", g.mutations);
        reads_for(5 * S);                       /* the chip ignores this clear too */
        CHECK((regs[9] & 0x08) && maint_gate_poll(&g) == MAINT_GATE_WAIT, "still dirty, still waiting");
        reads_for(5 * S);                       /* pending: recovery runs and is proven */
        CHECK(!(regs[9] & 0x08) && g.mutations == 0 && bg_writes_busy == 0, "recovered while pending");
        CHECK(maint_gate_poll(&g) == MAINT_GATE_GRANTED, "then granted");
        reads_for(60 * S);
        CHECK(bg_writes_busy == 0 && dpdm_writes_busy == 0, "no BG_EN edge under granted maintenance");
        maint_gate_end(&g);
        CHECK(!locked && !mutex_held && in_mutation == 0 && g.mutations == 0, "clean exit");
        puts("OK");
        return 0;
    }
    if (!strcmp(c, "reboot_dpdm") || !strcmp(c, "reboot_bg_en") ||
        !strcmp(c, "boot_read_failure") || !strcmp(c, "boot_clean")) {
        /* PR #112 re-review P1: an ESP-only reboot clears every RAM flag and
         * the gate, but the separately powered MP2731 keeps REG09/REG0B. The
         * driver must rebuild the quarantine from the registers at init, in
         * either wiring order, before maintenance can be granted. */
        for (int gate_first = 0; gate_first < 2; gate_first++) {
            fresh();
            bg_writes_busy = dpdm_writes_busy = 0;
            if (!strcmp(c, "reboot_dpdm")) {
                ok_0b_writes = 1;                 /* force lands, restore fails */
                CHECK(m_dpdm() != ESP_OK && (regs[0x0B] & 0x20), "dpdm left set");
                ok_0b_writes = 0;                 /* and the bus stays bad across the reboot */
            } else if (!strcmp(c, "reboot_bg_en")) {
                bg_stuck = 1 + (int)MP2731_BG_EN_CLEAR_RETRIES + 1;
                CHECK(m_bg_en() != ESP_OK && (regs[9] & 0x08), "bg_en left set");
            } else if (!strcmp(c, "boot_read_failure")) {
                fail_reads_09 = 2;                /* the boot read (and nothing else) fails */
            }
            /* ── ESP reboot: RAM (BSS) gone, charger registers retained ── */
            memset(&g, 0, sizeof g); in_mutation = 0;
            s_quarantine = false; s_bg_en_dirty = false; s_dpdm_dirty = false;
            s_manual_active = false; s_marker_checked = false; s_prev_adc_valid = false;
            s_gate_begin = NULL; s_gate_end = NULL; s_redetect_veto = NULL;
            if (!strcmp(c, "boot_read_failure")) fail_reads_09 = 1;
            const int boot_writes = writes;
            if (gate_first) mp2731_set_mutation_gate(gate_begin, gate_end);
            assert(mp2731_init() == ESP_OK);
            mp2731_redetect_set_veto(veto);
            if (!gate_first) mp2731_set_mutation_gate(gate_begin, gate_end);
            CHECK(writes == boot_writes, "boot writes nothing to the charger");
            const maint_gate_result_t r = maint_gate_request(&g);
            if (!strcmp(c, "boot_clean")) {
                CHECK(r == MAINT_GATE_GRANTED && g.mutations == 0,
                      "healthy charger: maintenance granted at once (gate_first %d)", gate_first);
                maint_gate_end(&g);
                continue;
            }
            CHECK(r == MAINT_GATE_WAIT, "%s: maintenance granted after the reboot with REG09 0x%02X "
                  "REG0B 0x%02X (gate_first %d)", c, regs[9], regs[0x0B], gate_first);
            CHECK(g.mutations == 1, "the rebuilt quarantine holds the gate (%u)", g.mutations);
            if (!strcmp(c, "reboot_dpdm")) {
                reads_for(20 * S);
                CHECK(maint_gate_poll(&g) == MAINT_GATE_WAIT && (regs[0x0B] & 0x20), "still unproven");
                ok_0b_writes = -1;
            }
            for (int i = 0; i < 4 && g.mutations != 0; i++) reads_for(5 * S);
            CHECK(!(regs[9] & 0x08) && !(regs[0x0B] & 0x20) && g.mutations == 0,
                  "%s: recovery proved both bits 0 (REG09 0x%02X REG0B 0x%02X, %u)", c, regs[9],
                  regs[0x0B], g.mutations);
            CHECK(maint_gate_poll(&g) == MAINT_GATE_GRANTED, "%s: granted after recovery", c);
            reads_for(30 * S);
            CHECK(bg_writes_busy == 0 && dpdm_writes_busy == 0, "%s: no REG09/REG0B write under maintenance", c);
            maint_gate_end(&g);
            CHECK(!locked && !mutex_held && in_mutation == 0 && g.mutations == 0, "clean exit");
        }
        puts("OK");
        return 0;
    }
    if (!strcmp(c, "no_edge_under_maintenance")) {
        /* Whatever maintenance is told after a failed cleanup, no read path
         * may write REG09/REG0B while it RUNS. The request is not checked
         * here: reads continue as on the device (on v2.23.0+#112 maintenance
         * was granted at once and the next read wrote the 1->0 edge). */
        for (int bit = 0; bit < 2; bit++) {
            fresh();
            bg_writes_busy = dpdm_writes_busy = 0;
            if (bit == 0) { bg_stuck = 1 + (int)MP2731_BG_EN_CLEAR_RETRIES; (void)m_bg_en(); }
            else { ok_0b_writes = 1; (void)m_dpdm(); ok_0b_writes = -1; }
            maint_gate_result_t r = maint_gate_request(&g);
            for (int i = 0; i < 6; i++) {
                reads_for(5 * S);
                if (r == MAINT_GATE_WAIT) r = maint_gate_poll(&g);
            }
            CHECK(bg_writes_busy == 0 && dpdm_writes_busy == 0,
                  "%s: REG09/REG0B written under granted maintenance (%d, %d)",
                  bit ? "dpdm" : "bg_en", bg_writes_busy, dpdm_writes_busy);
            CHECK(r == MAINT_GATE_GRANTED && !(regs[9] & 0x08) && !(regs[0x0B] & 0x20),
                  "%s: granted only once recovered", bit ? "dpdm" : "bg_en");
            maint_gate_end(&g);
            CHECK(g.mutations == 0 && in_mutation == 0, "clean");
        }
        puts("OK");
        return 0;
    }
    if (!strcmp(c, "dpdm_cleanup")) {
        /* HAR-310 #1, the Codex repro: maintenance is requested during a
         * manual dpdm and the +5 s snapshot's mutex take times out (takes:
         * 1 pulse, 2 +1.5 s, 3 +5 s). When maintenance is finally granted,
         * REG0B bit 5 must already be back at 0, restored inside the
         * admitted mutation (the write CHECK above enforces the inside). */
        fresh();
        inject_at = 1; takes = 0; take_fail_at = 3;
        const esp_err_t r = m_dpdm();
        CHECK(injected == MAINT_GATE_WAIT && g.pending, "maintenance waited for the pulse");
        CHECK(maint_gate_poll(&g) == MAINT_GATE_GRANTED, "granted after the mutation ended");
        CHECK(!(regs[0x0B] & 0x20), "maintenance granted with REG0B bit 5 asserted (0x%02X, return %d)",
              regs[0x0B], r);
        CHECK(r == ESP_OK && rep.cleared && rep.snapshots == 3, "verified cleanup, one snapshot missing");
        maint_gate_end(&g);
        CHECK(!locked && !mutex_held && in_mutation == 0 && g.mutations == 0, "clean exit");
        puts("OK");
        return 0;
    }
    for (unsigned i = 0; i < sizeof M / sizeof *M; i++) {
        const mutation_t *m = &M[i];
        if (!strcmp(c, "each_step")) {
            /* Count the mutation's I/O steps, then inject at every one. */
            fresh();
            CHECK(m->run() == ESP_OK, "%s baseline", m->name);
            const int steps = ops_in_window;
            CHECK(steps > 0 && writes > 0 && g.mutations == 0, "%s did I/O (%d)", m->name, steps);
            for (int k = 1; k <= steps; k++) {
                fresh();
                inject_at = k;
                CHECK(m->run() == ESP_OK, "%s completes with maintenance waiting at step %d", m->name, k);
                CHECK(injected == MAINT_GATE_WAIT && g.pending && !g.busy && g.mutations == 0,
                      "%s step %d: maintenance pending, not granted", m->name, k);
                /* Pending refuses new mutations: maintenance cannot be starved. */
                const int ops = bus_ops;
                CHECK(m_vin() == ESP_ERR_NOT_ALLOWED && bus_ops == ops, "%s step %d: new mutation refused", m->name, k);
                CHECK(maint_gate_poll(&g) == MAINT_GATE_GRANTED && g.busy, "%s step %d: granted after", m->name, k);
                maint_gate_end(&g);
            }
            printf("%s: %d I/O steps, maintenance waited at every one\n", m->name, steps);
        } else if (!strcmp(c, "held")) {
            fresh();
            CHECK(maint_gate_request(&g) == MAINT_GATE_GRANTED, "maintenance granted");
            const int ops = bus_ops, w = writes;
            const esp_err_t r = m->run();
            CHECK(r == ESP_ERR_NOT_ALLOWED && writes == w, "%s refused/held under maintenance (%d)", m->name, r);
            if (!m->automatic) CHECK(bus_ops == ops, "%s: not even a bus read", m->name);
            /* Probing paths stop at the veto probe; the others at the gate. */
            if (!m->probes) CHECK(g.refused >= 1, "%s: refusal counted by the gate", m->name);
            maint_gate_end(&g);
            if (m->automatic) {
                CHECK(m->run() == ESP_OK, "%s runs once maintenance ends", m->name);
            } else {
                CHECK(m->run() == ESP_OK, "%s admitted after maintenance", m->name);
            }
        } else if (!strcmp(c, "race")) {
            if (!m->probes) continue;
            fresh();
            if (!strcmp(m->name, "aico_auto")) {
                regs[2] = 0xDD; reads_for(5 * S); s_aico.next_ms = uptime_ms();
                const int w = writes, ops_before = bus_ops;
                race_after_probe = 1;
                reads_for(5 * S);
                (void)ops_before;
                CHECK(writes == w && s_aico.resets == 0 && s_aico.vetoed == 1 && g.busy,
                      "aico_auto: probe said free, maintenance began, nothing written");
                maint_gate_end(&g);
                reads_for(5 * S);
                CHECK(s_aico.resets == 1, "re-armed after maintenance");
            } else if (!strcmp(m->name, "redetect_auto")) {
                for (int n = 0; n < 60 && !s_crd.dwelling; n++) reads_for(5 * S);
                /* Arm the race for the read where the dwell completes. */
                while (clock_us / 1000 - s_crd.dwell_start_ms < CRD_DWELL_MS - 5 * S) reads_for(5 * S);
                const int w = writes;
                race_after_probe = 1;
                reads_for(10 * S);
                CHECK(writes == w && s_crd.attempts == 0 && s_crd.vetoed >= 1 && g.busy,
                      "redetect_auto: probe said free, maintenance began, no pulse (attempts %u)", s_crd.attempts);
                maint_gate_end(&g);
                CHECK(m_redetect_auto() == ESP_OK && s_crd.attempts == 1, "pulses after maintenance");
            } else {
                const int w = writes;
                race_after_probe = 1;
                CHECK(m->run() == ESP_ERR_NOT_ALLOWED && writes == w, "%s refused after the probe", m->name);
                maint_gate_end(&g);
            }
        } else if (!strcmp(c, "abandon")) {
            fresh();
            inject_at = 1;
            CHECK(m->run() == ESP_OK && g.pending, "%s pending", m->name);
            maint_gate_abandon(&g);            /* the worker gave up after its bound */
            CHECK(!maint_gate_active(&g) && m_iin() == ESP_OK, "%s: mutations admitted again", m->name);
            CHECK(maint_gate_request(&g) == MAINT_GATE_GRANTED, "%s: a later request is granted", m->name);
            maint_gate_end(&g);
        } else {
            fprintf(stderr, "unknown case %s\n", c); return 2;
        }
        CHECK(!locked && !mutex_held && in_mutation == 0 && g.mutations == 0, "%s: clean exit", m->name);
    }
    puts("OK");
    return 0;
}
