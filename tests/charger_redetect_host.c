/* Charger re-detection: the production MP2731 adapter (included as this
 * translation unit so the reset guard's RTC word is reachable), the core
 * pulse logic, the CLI parser and the asynchronous MQTT handler, against a
 * fake charger at the I2C boundary. The device_commands port shim delegates
 * straight to the driver; only I2C, time, worker submission and the reply
 * transport are faked. usage: charger_redetect_host <case> (exit 0 = pass) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/semphr.h"

#include "../components/mp2731/mp2731.c"

#include "cli_charger_redetect.h"
#include "charger_redetect_command.h"
#include "device_commands.h"

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

/* ── Fake charger ─────────────────────────────────────────────────────── */
static uint8_t regs[24];
static int64_t clock_us = 1000000;
static int mutex_held, locked, sessions, set_session = -1;
static int bg_edges, bg_set_writes, bg_clear_writes, dpdm_writes, reg03_writes;
static bool bg_ignore, dpdm_keep, vary, model_detect;
static int bg_stuck_clears;          /* clears to refuse while bit 3 is set */
static uint8_t detect_status = 0x30; /* REG0C after a BG_EN 1->0 edge */
static int fail_write_reg = -1, fail_write_n, fail_read_reg = -1, fail_read_n;
static esp_err_t lock_error;
static esp_reset_reason_t reset_reason = ESP_RST_POWERON;
static bool veto_on;
static bool relaxed_session;   /* a deferred clear (dirty BG_EN) runs in a later session */
static unsigned conversions;
/* HAR-310 injections. */
static int takes, take_fail_at = -1;  /* one-shot: the Nth xSemaphoreTake times out */
static int fail_0b_skip;              /* REG0B writes that pass before fail_write_reg applies */
static int dpdm_stuck_clears;         /* REG0B clears of bit 5 the chip ignores */
static bool lock_fail_after_r17;      /* the next bus lock after a REG17 read fails */
static bool lock_fail_next;

esp_reset_reason_t esp_reset_reason(void) { return reset_reason; }
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
/* The ADC poll sleeps 5 ms under the read mutex; a re-detect settle wait
 * (>= 100 ms) must never hold it, or every power read would stall behind it. */
void vTaskDelay(TickType_t ticks) { assert(ticks < 100 || !mutex_held); clock_us += (int64_t)ticks * 1000; }
const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "error"; }
esp_err_t i2c_bus_get_port(i2c_port_t *p) { *p = 0; return ESP_OK; }
esp_err_t i2c_bus_lock(unsigned timeout)
{
    assert(timeout > 0 && !locked);
    if (lock_error) return lock_error;
    if (lock_fail_next) { lock_fail_next = false; return ESP_ERR_TIMEOUT; }
    locked = 1; sessions++; return ESP_OK;
}
esp_err_t i2c_bus_unlock(void) { assert(locked); locked = 0; return ESP_OK; }
esp_err_t i2c_bus_probe_addr(i2c_port_t p, unsigned a) { (void)p; assert(a == 0x4B); return ESP_OK; }
esp_err_t i2c_master_write_read_device(int port, unsigned addr, const uint8_t *w, size_t wn,
                                       uint8_t *r, size_t rn, unsigned timeout)
{
    (void)port; (void)timeout;
    assert(locked && addr == 0x4B && wn == 1 && rn == 1 && *w < 24);
    if (*w == fail_read_reg && fail_read_n > 0) { fail_read_n--; return ESP_FAIL; }
    if (*w == 0x17 && lock_fail_after_r17) { lock_fail_after_r17 = false; lock_fail_next = true; }
    *r = regs[*w];
    return ESP_OK;
}
esp_err_t i2c_master_write_to_device(int port, unsigned addr, const uint8_t *w, size_t wn, unsigned timeout)
{
    (void)port; (void)timeout;
    assert(locked && addr == 0x4B && wn == 2);
    if (w[0] == fail_write_reg && fail_write_n > 0) {
        if (w[0] == 0x0B && fail_0b_skip > 0) fail_0b_skip--;
        else { fail_write_n--; return ESP_FAIL; }
    }
    const uint8_t v = w[1];
    switch (w[0]) {
    case 0x03:   /* ADC control: a START converts instantly */
        reg03_writes++;
        regs[3] = v & 0x7F;
        if (v & 0x80) {
            conversions++;
            if (vary) { regs[0x11] = (uint8_t)(124 + conversions % 2); regs[0x0F] = (uint8_t)(175 + conversions % 2); }
        }
        return ESP_OK;
    case 0x09: {
        const bool set = (v & 0x08) != 0, was = (regs[9] & 0x08) != 0;
        if (set) bg_set_writes++; else bg_clear_writes++;
        if (bg_ignore) { regs[9] = v & (uint8_t)~0x08; return ESP_OK; }
        if (was && !set && bg_stuck_clears > 0) { bg_stuck_clears--; return ESP_OK; }
        regs[9] = v;
        if (set) set_session = sessions;
        if (was && !set) {
            bg_edges++;
            CHECK(relaxed_session || sessions == set_session, "BG_EN 1 and 0 under ONE bus lock (set %d, clear %d)",
                  set_session, sessions);
            if (model_detect) regs[0x0C] = detect_status;
        }
        return ESP_OK;
    }
    case 0x0B:
        dpdm_writes++;
        CHECK((v & 0x19) == 0, "DM/DP/FAST_CHG_RESET never written 1 (0x%02X)", v);
        if (!(v & 0x20) && (regs[0x0B] & 0x20) && dpdm_stuck_clears > 0) { dpdm_stuck_clears--; return ESP_OK; }
        regs[0x0B] = dpdm_keep ? v : (uint8_t)(v & ~0x20);
        return ESP_OK;
    default:
        CHECK(false, "write to REG%02X: only REG03/09/0B may ever be written", w[0]);
    }
    return ESP_FAIL;
}

/* The 17 Nov stuck state. */
static void field_state(void)
{
    static const uint8_t field[24] = {
        [0x00] = 0x48, [0x01] = 0x06, [0x02] = 0xDC, [0x03] = 0x10, [0x04] = 0x5B,
        [0x08] = 0x9D, [0x09] = 0x05, [0x0B] = 0xC6, [0x0C] = 0x00, [0x0D] = 0x80,
        [0x0E] = 0, [0x0F] = 175, [0x10] = 102, [0x11] = 125, [0x12] = 0, [0x13] = 0,
        [0x14] = 0x08, [0x17] = 0x00,
    };
    memcpy(regs, field, sizeof regs);
}

static bool veto(void) { return veto_on; }

/* ── device_commands shim + frontends ─────────────────────────────────── */
static int cmd_calls;
cmd_result_t cmd_charger_redetect(power_redetect_method_t method, power_redetect_report_t *out)
{
    cmd_calls++;
    cmd_result_t r = {.status = mp2731_charger_redetect(method, out)};
    snprintf(r.message, sizeof r.message, "re-detect %s status=%d REG%02X 0x%02X->0x%02X->0x%02X",
             method == POWER_REDETECT_DPDM ? "dpdm" : "bg_en", r.status, out->reg,
             out->reg_before, out->reg_set, out->reg_after);
    return r;
}
cmd_result_t cmd_charger_redetect_auto(int set, power_redetect_auto_t *out)
{
    cmd_calls++;
    cmd_result_t r = {.status = mp2731_redetect_auto(set, out)};
    snprintf(r.message, sizeof r.message, "auto %s state=%s", out->enabled ? "ON" : "OFF",
             out->state ? out->state : "?");
    return r;
}
static char last_reply[4096];
static void (*job)(void *);
static void *job_arg;
static int submits, reject_submit;
static void reply(const char *json) { assert(strlen(json) < sizeof last_reply); strcpy(last_reply, json); }
static bool submit(void (*run)(void *), void *arg)
{
    submits++;
    if (reject_submit) return false;
    assert(!job); job = run; job_arg = arg; return true;
}
static void request(const char *json)
{
    cJSON *root = cJSON_Parse(json); assert(root);
    assert(charger_redetect_command_handle(root)); cJSON_Delete(root);
}
static void execute(void)
{
    assert(job); void (*run)(void *) = job; job = NULL; run(job_arg);
    assert(!locked && !mutex_held);
}
static cJSON *response(bool ok)
{
    cJSON *root = cJSON_Parse(last_reply); CHECK(root, "%s", last_reply);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok")) == ok, "%s", last_reply);
    return root;
}
static void expect_error(const char *detail)
{
    cJSON *root = response(false);
    CHECK(strstr(cJSON_GetObjectItemCaseSensitive(root, "detail")->valuestring, detail), "%s", last_reply);
    cJSON_Delete(root);
}

/* One power read every 5 s for `ms`. */
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

static int render_has(const char *needle)
{
    char json[CHARGER_DIAGNOSTICS_JSON_CAP];
    uint64_t window = 0;
    CHECK(mp2731_render_diagnostics(json, sizeof json, &window, false) > 0, "render");
    return strstr(json, needle) != NULL;
}

static void boot(void)
{
    field_state();
    assert(mp2731_init() == ESP_OK);
    mp2731_redetect_set_veto(veto);
    charger_redetect_command_config_t cfg = {.reply = reply, .submit = submit};
    charger_redetect_command_init(&cfg);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *c = argv[1];
    if (!strcmp(c, "reset_guard")) {
        s_redetect_marker = REDETECT_MARKER_OPEN; reset_reason = ESP_RST_BROWNOUT;
        boot();
        CHECK(s_crd.disabled == CRD_DISABLED_RESET && s_redetect_marker == 0, "brownout during attempt");
        vary = true; model_detect = true;
        reads_for(30 * 60 * 1000);
        CHECK(bg_set_writes == 0, "no automatic pulse for the rest of this boot");
        power_redetect_auto_t st;
        CHECK(mp2731_redetect_auto(-1, &st) == ESP_OK && !st.enabled &&
              !strcmp(st.disabled_reason, "reset_during_attempt"), "reported");
        CHECK(mp2731_redetect_auto(1, &st) == ESP_OK && st.enabled, "operator may re-enable");
        reads_for(5 * 60 * 1000);
        CHECK(bg_edges == 1, "re-enabled pulses again (%d)", bg_edges);
        /* Deliberate restarts and power-on never disable. */
        const esp_reset_reason_t ok_reasons[] = {ESP_RST_POWERON, ESP_RST_SW};
        for (unsigned i = 0; i < 2; i++) {
            s_marker_checked = false; s_redetect_marker = REDETECT_MARKER_OPEN; reset_reason = ok_reasons[i];
            assert(mp2731_init() == ESP_OK);
            CHECK(s_crd.disabled == CRD_ENABLED, "reason %d keeps auto on", ok_reasons[i]);
        }
        /* A closed marker never disables, whatever the reset. */
        s_marker_checked = false; s_redetect_marker = 0; reset_reason = ESP_RST_PANIC;
        assert(mp2731_init() == ESP_OK);
        CHECK(s_crd.disabled == CRD_ENABLED, "closed marker");
        puts("OK"); return 0;
    }
    boot();
    power_redetect_report_t rep;
    if (!strcmp(c, "bg_en")) {
        model_detect = true;
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep) == ESP_OK, "pulse");
        CHECK(rep.reg == 0x09 && rep.reg_before == 0x05 && rep.reg_set == 0x0D && rep.reg_after == 0x05,
              "reserved bits preserved: 0x%02X 0x%02X 0x%02X", rep.reg_before, rep.reg_set, rep.reg_after);
        CHECK(rep.latched && rep.cleared && !rep.restored && regs[9] == 0x05, "latched + cleared");
        CHECK(bg_edges == 1 && bg_set_writes == 1 && bg_clear_writes == 1, "exactly one 1->0 edge");
        CHECK(rep.snapshots == 4, "four snapshots");
        CHECK(rep.snap[0].t_ms <= 0 && rep.snap[1].t_ms >= 0 && rep.snap[1].t_ms < 100, "before/immediate");
        CHECK(rep.snap[2].t_ms >= POWER_REDETECT_SETTLE1_MS && rep.snap[3].t_ms >= POWER_REDETECT_SETTLE2_MS,
              "settle points %d %d", rep.snap[2].t_ms, rep.snap[3].t_ms);
        for (unsigned i = 0; i < 4; i++) CHECK(rep.snap[i].mask == 0xFFFFFF, "full scan %u", i);
        CHECK(rep.snap[0].regs[0x0C] == 0x00 && rep.snap[1].regs[0x0C] == 0x30, "before/after REG0C");
        CHECK(s_crd.manual == 1 && s_crd.attempts == 0 && s_redetect_marker == 0 && !s_manual_active,
              "manual counted apart from auto; marker closed");
    } else if (!strcmp(c, "not_latched")) {
        bg_ignore = true;
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep) == ESP_ERR_INVALID_RESPONSE, "no latch");
        CHECK(!rep.latched && rep.snapshots == 2 && bg_edges == 0 && bg_set_writes == 1 &&
              bg_clear_writes == 0, "failure, nothing to clear, no settle wait");
    } else if (!strcmp(c, "stuck")) {
        bg_stuck_clears = 1 + MP2731_BG_EN_CLEAR_RETRIES + 1;   /* survives the pulse and one read */
        relaxed_session = true;
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep) == ESP_ERR_INVALID_STATE, "stuck");
        CHECK(rep.latched && !rep.cleared && (regs[9] & 0x08) && s_bg_en_dirty, "reported stuck");
        CHECK(bg_clear_writes == 1 + (int)MP2731_BG_EN_CLEAR_RETRIES, "bounded clears (%d)", bg_clear_writes);
        power_redetect_auto_t st;
        CHECK(mp2731_redetect_auto(1, &st) == ESP_ERR_INVALID_STATE, "no auto over a set BG_EN");
        power_reading_t p;
        (void)mp2731_read_power(&p);
        CHECK(s_bg_en_dirty && (regs[9] & 0x08), "still refused once");
        (void)mp2731_read_power(&p);
        CHECK(!s_bg_en_dirty && regs[9] == 0x05, "the next read clears it");
        CHECK(mp2731_redetect_auto(1, &st) == ESP_OK && st.enabled, "auto allowed again");
    } else if (!strcmp(c, "bus")) {
        lock_error = ESP_ERR_TIMEOUT;
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep) == ESP_ERR_TIMEOUT, "lock");
        CHECK(bg_set_writes == 0 && !s_bg_en_dirty, "nothing written");
        lock_error = ESP_OK;
        fail_write_reg = 0x09; fail_write_n = 1;
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep) == ESP_FAIL, "set write failed");
        CHECK(bg_clear_writes == 1 && rep.cleared && !s_bg_en_dirty && regs[9] == 0x05,
              "the clear still ran and verified");
        fail_read_reg = 0x09; fail_read_n = 2;   /* before-read ok? no: first read fails */
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep) != ESP_OK, "read failure");
        fail_read_n = 0;
        /* Every clear fails on the bus: dirty, cleared later by a read. */
        const int edges = bg_edges;
        fail_write_reg = 0x09; fail_write_n = 1 + 1 + (int)MP2731_BG_EN_CLEAR_RETRIES;
        regs[9] = 0x0D;   /* pretend the set landed before the bus died */
        relaxed_session = true;
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep) != ESP_OK && s_bg_en_dirty, "dirty");
        power_reading_t p;
        (void)mp2731_read_power(&p);
        CHECK(!s_bg_en_dirty && regs[9] == 0x05 && bg_edges == edges + 1, "cleared on the next read");
    } else if (!strcmp(c, "dpdm")) {
        dpdm_keep = true;
        CHECK(mp2731_charger_redetect(POWER_REDETECT_DPDM, &rep) == ESP_OK, "dpdm");
        CHECK(rep.reg == 0x0B && rep.reg_before == 0xC6 && rep.reg_set == 0xE6 && rep.latched, "forced");
        CHECK(rep.restored && rep.reg_after == 0xC6 && regs[0x0B] == 0xC6 && rep.cleared, "restored");
        CHECK(rep.snapshots == 4 && dpdm_writes == 2 && bg_set_writes == 0, "only REG0B touched");
        dpdm_keep = false; dpdm_writes = 0;
        CHECK(mp2731_charger_redetect(POWER_REDETECT_DPDM, &rep) == ESP_OK, "self-clearing");
        CHECK(!rep.latched && !rep.restored && rep.cleared && dpdm_writes == 1, "no restore needed");
    } else if (!strcmp(c, "dpdm_settle_timeout")) {
        /* HAR-310 #1 (Codex repro): the +5 s snapshot's mutex take times out.
         * Takes: 1 = pulse, 2 = +1.5 s, 3 = +5 s. The restore must still run. */
        dpdm_keep = true; takes = 0; take_fail_at = 3;
        const esp_err_t err = mp2731_charger_redetect(POWER_REDETECT_DPDM, &rep);
        CHECK(!(regs[0x0B] & 0x20), "REG0B bit 5 left asserted: 0x%02X (return %d)", regs[0x0B], err);
        CHECK(err == ESP_OK && rep.latched && rep.restored && rep.cleared && rep.reg_after == 0xC6,
              "restored and verified: err %d restored %d cleared %d after 0x%02X",
              err, rep.restored, rep.cleared, rep.reg_after);
        CHECK(rep.snapshots == 3, "the skipped snapshot is only missing evidence (%u)", rep.snapshots);
        CHECK(!s_manual_active && s_redetect_marker == 0, "closed");
    } else if (!strcmp(c, "dpdm_restore_io")) {
        /* The restore write fails on the bus: never success, fail closed. */
        dpdm_keep = true; fail_write_reg = 0x0B; fail_0b_skip = 1; fail_write_n = 1000;
        const esp_err_t err = mp2731_charger_redetect(POWER_REDETECT_DPDM, &rep);
        CHECK(err != ESP_OK && !rep.cleared, "unverified restore reported as success (%d)", err);
        CHECK(regs[0x0B] & 0x20, "bus dead: bit 5 still set");
        /* Pulses of any kind are refused while it is not proven 0. */
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep) == ESP_ERR_NOT_FINISHED &&
              bg_set_writes == 0, "manual pulse over a dirty REG0B must be refused");
        power_redetect_auto_t st;
        CHECK(mp2731_redetect_auto(1, &st) == ESP_ERR_INVALID_STATE, "auto on refused while dirty");
        CHECK(render_has("\"dirty\":[\"dpdm\"]"), "telemetry shows the dirty bit");
        vary = true; model_detect = true;
        reads_for(10 * 60 * 1000);
        CHECK(bg_set_writes == 0, "no automatic pulse while dirty");
        /* Bus back: the next power read restores it, then pulses are allowed. */
        fail_write_n = 0;
        reads_for(5 * 1000);
        CHECK(!(regs[0x0B] & 0x20), "a power read must retry and prove the restore (0x%02X)", regs[0x0B]);
        CHECK(render_has("\"dirty\":[]"), "clean again");
        CHECK(mp2731_redetect_auto(1, &st) == ESP_OK, "auto on allowed again");
        reads_for(5 * 60 * 1000);
        CHECK(bg_edges == 1, "automatic pulse resumes after recovery (%d)", bg_edges);
    } else if (!strcmp(c, "dpdm_restore_mismatch")) {
        /* The restore write lands but bit 5 reads back 1 (twice). */
        dpdm_keep = true; dpdm_stuck_clears = 2;
        const esp_err_t err = mp2731_charger_redetect(POWER_REDETECT_DPDM, &rep);
        CHECK(err == ESP_ERR_INVALID_STATE && !rep.cleared && (rep.reg_after & 0x20),
              "readback mismatch is a failure (%d, 0x%02X)", err, rep.reg_after);
        power_reading_t p;
        (void)mp2731_read_power(&p);
        CHECK(regs[0x0B] & 0x20, "chip still refuses once");
        (void)mp2731_read_power(&p);
        CHECK(!(regs[0x0B] & 0x20), "the next read restores it (0x%02X)", regs[0x0B]);
        CHECK(mp2731_charger_redetect(POWER_REDETECT_DPDM, &rep) == ESP_OK && rep.cleared,
              "pulses allowed again once clean");
    } else if (!strcmp(c, "auto_bus_unavailable")) {
        /* HAR-310 #2: the precheck passes, then the pulse's bus lock fails.
         * Nothing was written: no dirty flag, a failed attempt, no pulse. */
        vary = true; model_detect = true;
        reads_for(170 * 1000);
        lock_fail_after_r17 = true;
        /* The uninitialised read this guards against is invisible to ASan:
         * the MSan build (test_charger_redetect.MsanDriverTest) catches it. */
        for (int i = 0; i < 4 && s_crd.attempts == 0; i++) {
            power_reading_t p;
            (void)mp2731_read_power(&p);
            clock_us += 5000000;
        }
        CHECK(s_crd.attempts == 1 && s_crd.failed == 1 && !s_crd.pending, "one failed attempt");
        CHECK(!strcmp(crd_outcome_name(s_crd.last_outcome), "bus_error"), "outcome %s",
              crd_outcome_name(s_crd.last_outcome));
        CHECK(!s_bg_en_dirty && bg_set_writes == 0 && bg_clear_writes == 0, "nothing written, nothing dirty");
        reads_for(5 * 60 * 1000);
        CHECK(bg_set_writes == 0, "backoff holds the next attempt");
    } else if (!strcmp(c, "veto")) {
        veto_on = true;
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, &rep) == ESP_ERR_NOT_ALLOWED, "veto");
        CHECK(mp2731_charger_redetect(POWER_REDETECT_DPDM, &rep) == ESP_ERR_NOT_ALLOWED, "veto");
        CHECK(sessions == 0, "not even a bus transaction");
        CHECK(mp2731_charger_redetect(7, &rep) == ESP_ERR_INVALID_ARG, "bad method");
        CHECK(mp2731_charger_redetect(POWER_REDETECT_BG_EN, NULL) == ESP_ERR_INVALID_ARG, "null");
        /* Automatic: held for the whole lock, fires once it is released. */
        vary = true;
        reads_for(20 * 60 * 1000);
        CHECK(bg_set_writes == 0 && s_crd.vetoed == 1, "held, counted once");
        veto_on = false;
        reads_for(10 * 1000);
        CHECK(bg_edges == 1, "fires after the lock");
    } else if (!strcmp(c, "auto")) {
        vary = true; model_detect = true;
        reads_for(170 * 1000);
        CHECK(bg_set_writes == 0, "nothing inside the dwell");
        reads_for(15 * 1000);   /* reads at 170, 175, 180 s: the 180 s read fires */
        CHECK(bg_edges == 1 && s_crd.attempts == 1 && s_crd.pending, "one pulse after the dwell");
        CHECK(s_redetect_marker == REDETECT_MARKER_OPEN, "marker open while the outcome is pending");
        reads_for(10 * 1000);
        CHECK(!s_crd.pending && s_crd.last_outcome == CRD_OUT_CHARGING && s_redetect_marker == 0,
              "resolved: charging, marker closed");
        reads_for(60 * 60 * 1000);
        CHECK(bg_edges == 1, "charging units are never pulsed");
        char json[CHARGER_DIAGNOSTICS_JSON_CAP];
        uint64_t window = 0;
        const int n = mp2731_render_diagnostics(json, sizeof json, &window, false);
        CHECK(n > 0 && (size_t)n == strlen(json), "render");
        printf("CHARGER=%s\n", json);
        /* A tight buffer drops the optional objects whole (the last appended,
         * "aico", first), never the charger evidence. */
        const int tight = n - 30;
        char *buf = malloc((size_t)tight);
        CHECK(buf && mp2731_render_diagnostics(buf, (size_t)tight, &window, false) > 0 &&
              strstr(buf, "\"aico\"") == NULL && strstr(buf, "\"redetect\"") != NULL &&
              buf[strlen(buf) - 1] == '}', "fallback without aico");
        free(buf);
        const char *at = strstr(json, ",\"redetect\":");
        CHECK(at != NULL, "redetect present");
        const size_t tighter = (size_t)(at - json) + 20u;
        buf = malloc(tighter);
        CHECK(buf && mp2731_render_diagnostics(buf, tighter, &window, false) > 0 &&
              strstr(buf, "\"redetect\"") == NULL && strstr(buf, "\"aico\"") == NULL &&
              strstr(buf, "\"read_mask\"") != NULL, "fallback without redetect");
        free(buf);
    } else if (!strcmp(c, "safety_timer")) {
        vary = true; regs[0x17] = 0x80;
        reads_for(40 * 60 * 1000);
        CHECK(bg_set_writes == 0 && s_crd.attempts == 0 && s_crd.blocked >= 3 &&
              s_crd.last_block == CRD_BLOCK_SAFETY_TIMER, "never defeated, rechecked (%u)", s_crd.blocked);
        regs[0x17] = 0x00;
        reads_for(11 * 60 * 1000);
        CHECK(bg_edges == 1, "pulses once the timer fault is gone");
    } else if (!strcmp(c, "cli")) {
        char *a0[] = {"charger_redetect"};
        CHECK(cli_cmd_charger_redetect(1, a0) == 0 && bg_edges == 1, "default bg_en");
        char *a1[] = {"charger_redetect", "dpdm"};
        CHECK(cli_cmd_charger_redetect(2, a1) == 0 && dpdm_writes >= 1, "dpdm");
        char *a2[] = {"charger_redetect", "auto"};
        CHECK(cli_cmd_charger_redetect(2, a2) == 0, "query");
        char *a3[] = {"charger_redetect", "auto", "off"};
        CHECK(cli_cmd_charger_redetect(3, a3) == 0 && s_crd.disabled == CRD_DISABLED_OPERATOR, "off");
        char *a4[] = {"charger_redetect", "auto", "on"};
        CHECK(cli_cmd_charger_redetect(3, a4) == 0 && s_crd.disabled == CRD_ENABLED, "on");
        const int calls = cmd_calls, edges = bg_edges;
        char *bad1[] = {"charger_redetect", "bogus"};
        char *bad2[] = {"charger_redetect", "bg_en", "x"};
        char *bad3[] = {"charger_redetect", "auto", "maybe"};
        char *bad4[] = {"charger_redetect", "auto", "on", "x"};
        CHECK(cli_cmd_charger_redetect(2, bad1) == 1 && cli_cmd_charger_redetect(3, bad2) == 1 &&
              cli_cmd_charger_redetect(3, bad3) == 1 && cli_cmd_charger_redetect(4, bad4) == 1, "usage");
        CHECK(cmd_calls == calls && bg_edges == edges, "bad syntax never reaches the charger");
    } else if (!strcmp(c, "mqtt")) {
        model_detect = true;
        request("{\"type\":\"charger_redetect\",\"id\":\"r-\\\"1\\\"\"}");
        CHECK(job && sessions == 0 && cmd_calls == 0, "deferred: no I2C on the MQTT task");
        execute();
        printf("REPLY=%s\n", last_reply);
        cJSON *root = response(true);
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "id")->valuestring, "r-\"1\""), "id");
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "method")->valuestring, "bg_en"), "default");
        cJSON *snaps = cJSON_GetObjectItemCaseSensitive(root, "snapshots");
        CHECK(cJSON_GetArraySize(snaps) == 4, "four snapshots");
        CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(snaps, 0), "vin_stat")->valueint == 0 &&
              cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(snaps, 1), "chg_stat")->valueint == 2,
              "decoded before/after");
        cJSON_Delete(root);
        request("{\"type\":\"charger_redetect\",\"id\":\"d\",\"method\":\"dpdm\"}"); execute();
        root = response(true);
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "method")->valuestring, "dpdm"), "dpdm");
        cJSON_Delete(root);
        request("{\"type\":\"charger_redetect_auto\",\"id\":\"a\",\"enabled\":false}"); execute();
        root = response(true);
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring, "charger_redetect_auto_result") &&
              cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "enabled")) &&
              !strcmp(cJSON_GetObjectItemCaseSensitive(root, "disabled_reason")->valuestring, "operator"), "off");
        cJSON_Delete(root);
        request("{\"type\":\"charger_redetect_auto\",\"id\":\"q\"}"); execute();
        root = response(true);
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "enabled")), "query does not change it");
        cJSON_Delete(root);
        /* Failure evidence is still reported. */
        bg_ignore = true;
        request("{\"type\":\"charger_redetect\",\"id\":\"f\"}"); execute();
        root = response(false);
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "latched")) &&
              cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "snapshots")) == 2, "evidence");
        cJSON_Delete(root);
    } else if (!strcmp(c, "mqtt_invalid")) {
        request("{\"type\":\"charger_redetect\"}"); expect_error("invalid_id");
        request("{\"type\":\"charger_redetect\",\"id\":\"\"}"); expect_error("invalid_id");
        char json[256], id[66]; memset(id, 'x', 65); id[65] = '\0';
        snprintf(json, sizeof json, "{\"type\":\"charger_redetect\",\"id\":\"%s\"}", id);
        request(json); expect_error("invalid_id");
        request("{\"type\":\"charger_redetect\",\"id\":\"m\",\"method\":\"reset\"}"); expect_error("invalid_method");
        request("{\"type\":\"charger_redetect\",\"id\":\"m\",\"method\":1}"); expect_error("invalid_method");
        request("{\"type\":\"charger_redetect_auto\",\"id\":\"e\",\"enabled\":\"yes\"}"); expect_error("invalid_enabled");
        request("{\"type\":\"charger_redetect_auto\",\"id\":\"e\",\"enabled\":1}"); expect_error("invalid_enabled");
        CHECK(submits == 0 && sessions == 0, "nothing dispatched");
        request("{\"type\":\"charger_redetect\",\"id\":\"1\"}");
        request("{\"type\":\"charger_redetect_auto\",\"id\":\"2\"}"); expect_error("busy");
        execute();
        reject_submit = 1;
        request("{\"type\":\"charger_redetect\",\"id\":\"3\"}"); expect_error("queue_unavailable");
        CHECK(!job, "nothing queued");
        reject_submit = 0;
        request("{\"type\":\"charger_redetect\",\"id\":\"4\"}"); execute();
        cJSON *root = response(true); cJSON_Delete(root);
        root = cJSON_Parse("{\"type\":\"get_vin_limit\",\"id\":\"x\"}");
        CHECK(!charger_redetect_command_handle(root), "other commands pass through"); cJSON_Delete(root);
    } else {
        fprintf(stderr, "unknown case %s\n", c); return 2;
    }
    assert(!locked && !mutex_held && !job);
    puts("OK");
    return 0;
}
