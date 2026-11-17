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

esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
void h_critical_enter(portMUX_TYPE *m) { assert(!m->held); m->held = 1; }
void h_critical_exit(portMUX_TYPE *m) { assert(m->held); m->held = 0; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_held; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{
    (void)wait; assert(m == &mutex_held && !mutex_held); mutex_held = 1; return pdTRUE;
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
    case 0x0B: regs[0x0B] = dpdm_keep ? v : (uint8_t)(v & ~0x20); return ESP_OK;
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
