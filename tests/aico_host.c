/* Charger AICO: the production MP2731 adapter (included as this translation
 * unit so the re-detect and AICO state is reachable), the AICO policy, the
 * CLI parser and the asynchronous MQTT handler, against a fake charger at the
 * I2C boundary that models AICO (a REG00 write while AICO_EN=1 resets
 * IIN_DPM to IIN_LIM). The device_commands shim delegates straight to the
 * driver. usage: aico_host <case> (exit 0 = pass) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/semphr.h"

#include "../components/mp2731/mp2731.c"

#include "cli_aico.h"
#include "aico_command.h"
#include "device_commands.h"

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

#define S 1000LL
#define MIN (60 * S)

/* ── Fake charger ─────────────────────────────────────────────────────── */
static uint8_t regs[24];
static int64_t clock_us = 1000000;
static int mutex_held, locked, sessions;
static int reg00_writes, reg02_writes, reg02_reads, bg_writes;
static int read_session[24];          /* bus session of the last read of each register */
static int bg_session = -1, last_reg00_write_session = -1;
static int fail_write_reg = -1, fail_write_n, fail_read_reg = -1, fail_read_n;
static uint8_t corrupt_reg = 0xFF;    /* this register reads back XOR 0x01 after a write */
static int reads_of[24];
/* Fail reads of this register that happen AFTER a verified REG00/REG02
 * write in the same bus hold (the status reads that follow a mutation). */
static int fail_after_write_reg = -1, fail_after_write_n, last_write_session = -1;
static esp_err_t lock_error;
static bool veto_on, vary, model_detect;
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
esp_err_t i2c_bus_lock(unsigned timeout)
{
    assert(timeout > 0 && !locked);
    if (lock_error) return lock_error;
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
    if (*w == fail_after_write_reg && fail_after_write_n > 0 && last_write_session == sessions) {
        /* a read after the write in the same hold; tests target only the
         * status registers, never the register whose readback verifies it */
        fail_after_write_n--; return ESP_FAIL;
    }
    reads_of[*w]++;
    if (*w == 0x02) reg02_reads++;
    read_session[*w] = sessions;
    *r = regs[*w];
    return ESP_OK;
}
esp_err_t i2c_master_write_to_device(int port, unsigned addr, const uint8_t *w, size_t wn, unsigned timeout)
{
    (void)port; (void)timeout;
    assert(locked && addr == 0x4B && wn == 2);
    if (w[0] == fail_write_reg && fail_write_n > 0) { fail_write_n--; return ESP_FAIL; }
    const uint8_t v = w[1];
    switch (w[0]) {
    case 0x00:   /* the re-arm: the byte goes back EXACTLY as read, in the same bus hold */
        CHECK(v == regs[0], "REG00 rewritten unchanged (0x%02X over 0x%02X)", v, regs[0]);
        CHECK(read_session[0] == sessions, "REG00 read and written under ONE bus hold");
        CHECK(sessions != bg_session, "never inside a BG_EN pulse");
        reg00_writes++; last_reg00_write_session = sessions; last_write_session = sessions;
        regs[0] = w[0] == corrupt_reg ? (uint8_t)(v ^ 0x01) : v;
        if (regs[2] & 0x01) regs[0x14] = (uint8_t)((regs[0x14] & 0xC0) | (v & 0x3F));
        return ESP_OK;
    case 0x02:   /* AICO_EN: only bit 0 may change, from a read in this hold */
        CHECK((v & 0xFE) == (regs[2] & 0xFE), "REG02 bits 7:1 preserved (0x%02X over 0x%02X)", v, regs[2]);
        CHECK(read_session[2] == sessions, "REG02 read and written under ONE bus hold");
        reg02_writes++; last_write_session = sessions;
        regs[2] = w[0] == corrupt_reg ? (uint8_t)(v ^ 0x01) : v;
        return ESP_OK;
    case 0x03:   /* ADC control: a START converts instantly */
        regs[3] = v & 0x7F;
        if (v & 0x80) {
            conversions++;
            if (vary) { regs[0x11] = (uint8_t)(124 + conversions % 2); regs[0x0F] = (uint8_t)(175 + conversions % 2); }
        }
        return ESP_OK;
    case 0x09:   /* BG_EN pulse (re-detect interplay) */
        bg_writes++; bg_session = sessions;
        CHECK(last_reg00_write_session != sessions, "no REG00 write inside a BG_EN pulse");
        if ((regs[9] & 0x08) && !(v & 0x08) && model_detect) regs[0x0C] = 0x30;
        regs[9] = v;
        return ESP_OK;
    default:
        CHECK(false, "write to REG%02X: only REG00/02/03/09 may be written here", w[0]);
    }
    return ESP_FAIL;
}

/* Bench-like USB state: charged, AICO off (the fleet's REG02=0xDC). */
static void usb_state(void)
{
    static const uint8_t usb[24] = {
        [0x00] = 0x52, [0x01] = 0x06, [0x02] = 0xDC, [0x03] = 0x10, [0x04] = 0x5B,
        [0x08] = 0x95, [0x09] = 0x00, [0x0B] = 0xC0, [0x0C] = 0x39, [0x0D] = 0x80,
        [0x0E] = 0xD2, [0x0F] = 0xDA, [0x10] = 0x88, [0x11] = 0x56, [0x12] = 0x00, [0x13] = 0x08,
        [0x14] = 0x12, [0x15] = 0x60, [0x17] = 0x00,
    };
    memcpy(regs, usb, sizeof regs);
}
/* The 17 Nov stuck state (re-detect fires after its dwell). */
static void field_state(void)
{
    static const uint8_t field[24] = {
        [0x00] = 0x48, [0x01] = 0x06, [0x02] = 0xDD, [0x03] = 0x10, [0x04] = 0x5B,
        [0x08] = 0x9D, [0x09] = 0x05, [0x0B] = 0xC6, [0x0C] = 0x00, [0x0D] = 0x80,
        [0x0F] = 175, [0x10] = 102, [0x11] = 125, [0x14] = 0x04, [0x17] = 0x00,
    };
    memcpy(regs, field, sizeof regs);
}

static bool veto(void) { return veto_on; }

/* ── device_commands shim + frontends ─────────────────────────────────── */
static int cmd_calls;
static cmd_result_t shim(esp_err_t status, const power_aico_t *a)
{
    cmd_result_t r = {.status = status};
    if (status == ESP_OK) {
        snprintf(r.message, sizeof r.message, "AICO %s REG02=0x%02X IIN=%u IIN_DPM=%u resets=%u",
                 a->enabled ? "on" : "off", a->reg02, a->iin_limit_ma, a->iin_dpm_ma, (unsigned)a->resets);
    } else {
        snprintf(r.message, sizeof r.message, "test_error=%d", status);
    }
    return r;
}
cmd_result_t cmd_get_aico(power_aico_t *out) { cmd_calls++; return shim(mp2731_get_aico(out), out); }
cmd_result_t cmd_set_aico(bool e, power_aico_t *out) { cmd_calls++; return shim(mp2731_set_aico(e, out), out); }
cmd_result_t cmd_reset_aico(power_aico_t *out) { cmd_calls++; return shim(mp2731_reset_aico(out), out); }

static char last_reply[2048];
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
    assert(aico_command_handle(root)); cJSON_Delete(root);
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
    if (!ok) CHECK(!cJSON_GetObjectItemCaseSensitive(root, "enabled") &&
                   !cJSON_GetObjectItemCaseSensitive(root, "reg02"), "no readback on failure: %s", last_reply);
    return root;
}
static void expect_error(const char *detail)
{
    cJSON *root = response(false);
    CHECK(strstr(cJSON_GetObjectItemCaseSensitive(root, "detail")->valuestring, detail), "%s", last_reply);
    cJSON_Delete(root);
}
static int num(cJSON *root, const char *k)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(root, k); CHECK(cJSON_IsNumber(i), "%s", k); return i->valueint;
}
static bool flag(cJSON *root, const char *k)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(root, k); CHECK(cJSON_IsBool(i), "%s", k); return cJSON_IsTrue(i);
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

static void boot(bool field)
{
    if (field) field_state(); else usb_state();
    assert(mp2731_init() == ESP_OK);
    sessions = 0;   /* boot reconciliation (REG09/REG0B reads) is not command traffic */
    mp2731_redetect_set_veto(veto);
    aico_command_config_t cfg = {.reply = reply, .submit = submit};
    aico_command_init(&cfg);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *c = argv[1];
    power_aico_t a;
    if (!strcmp(c, "interplay")) {
        /* AICO due exactly while the automatic re-detect pulses and judges its
         * outcome: the re-arm waits for the outcome, and never shares a bus
         * hold with the BG_EN pulse (asserted in the write fake). */
        boot(true);
        vary = true;   /* no modeled detection: the outcome window stays open 60 s */
        /* Read until the re-detect pulses (after its 3 min dwell). */
        for (int i = 0; i < 100 && !s_crd.pending; i++) reads_for(5 * S);
        CHECK(s_crd.pending && s_crd.attempts == 1 && bg_writes >= 2, "re-detect fired (%d writes)", bg_writes);
        s_aico.next_ms = uptime_ms();              /* the re-arm is due right now */
        reads_for(30 * S);
        CHECK(s_crd.pending, "outcome window still open");
        CHECK(reg00_writes == 0 && s_aico.vetoed == 1, "re-arm held while the outcome is pending");
        for (int i = 0; i < 20 && s_crd.pending; i++) reads_for(5 * S);
        CHECK(!s_crd.pending && reg00_writes == 1 && s_aico.resets == 1, "then re-armed once");
        CHECK(regs[0x14] == 0x08, "IIN_DPM back to IIN_LIM (0x%02X)", regs[0x14]);
        puts("OK"); return 0;
    }
    boot(false);
    if (!strcmp(c, "set")) {
        CHECK(mp2731_get_aico(&a) == ESP_OK && !a.enabled && a.reg02 == 0xDC && a.iin_limit_ma == 1000 &&
              a.iin_dpm_ma == 1000 && !a.aico_stat && a.resets == 0 && !a.has_reset, "fleet default read");
        CHECK(reg02_writes == 0, "get never writes");
        CHECK(mp2731_set_aico(true, &a) == ESP_OK && a.enabled && a.reg02 == 0xDD && regs[2] == 0xDD,
              "on: 0xDC -> 0xDD");
        CHECK(a.next_check_in_s == 3600, "a full hour of learning first (%u)", a.next_check_in_s);
        CHECK(mp2731_set_aico(true, &a) == ESP_OK && regs[2] == 0xDD, "idempotent");
        CHECK(mp2731_set_aico(false, &a) == ESP_OK && !a.enabled && regs[2] == 0xDC, "off: back to 0xDC");
        /* Bits 7:1 are whatever the chip holds (here not the fleet value). */
        regs[2] = 0x2A;
        CHECK(mp2731_set_aico(true, &a) == ESP_OK && regs[2] == 0x2B && a.reg02 == 0x2B, "only bit 0");
        CHECK(mp2731_set_aico(false, &a) == ESP_OK && regs[2] == 0x2A, "only bit 0 off");
        CHECK(reg00_writes == 0, "switching never touches REG00");
        /* Failures: nothing reported as success, nothing written blind. */
        const int w = reg02_writes;
        fail_read_reg = 0x02; fail_read_n = 1;
        CHECK(mp2731_set_aico(true, &a) == ESP_FAIL && reg02_writes == w, "no write without the read");
        fail_write_reg = 0x02; fail_write_n = 1;
        CHECK(mp2731_set_aico(true, &a) == ESP_FAIL, "write error");
        corrupt_reg = 0x02;
        CHECK(mp2731_set_aico(true, &a) == ESP_ERR_INVALID_RESPONSE, "readback mismatch");
        corrupt_reg = 0xFF;
        lock_error = ESP_ERR_TIMEOUT;
        CHECK(mp2731_set_aico(true, &a) == ESP_ERR_TIMEOUT && mp2731_get_aico(&a) == ESP_ERR_TIMEOUT, "lock");
        lock_error = ESP_OK;
        CHECK(mp2731_set_aico(true, NULL) == ESP_ERR_INVALID_ARG && mp2731_get_aico(NULL) == ESP_ERR_INVALID_ARG &&
              mp2731_reset_aico(NULL) == ESP_ERR_INVALID_ARG, "null");
    } else if (!strcmp(c, "auto")) {
        CHECK(mp2731_set_aico(true, &a) == ESP_OK, "on");
        regs[0x14] = 0x04;                          /* AICO learned 300 mA at dawn */
        regs[0x15] = 0xE0;                          /* AICO_STAT */
        const int r02 = reg02_reads;
        reads_for(60 * MIN - 5 * S);
        CHECK(reg00_writes == 0 && reg02_reads == r02, "nothing inside the hour, not even a REG02 read");
        reads_for(10 * S);
        CHECK(reg00_writes == 1 && regs[0] == 0x52 && regs[0x14] == 0x12, "re-armed: IIN_DPM 300 -> 1000 mA");
        CHECK(s_aico.resets == 1 && s_aico.has_reset, "counted");
        regs[0x14] = 0x06;
        reads_for(60 * MIN);
        CHECK(reg00_writes == 2 && regs[0x14] == 0x12, "hourly");
        CHECK(mp2731_get_aico(&a) == ESP_OK && a.enabled && a.resets == 2 && a.has_reset &&
              a.iin_dpm_ma == 1000 && a.aico_stat, "reported");
        char json[CHARGER_DIAGNOSTICS_JSON_CAP];
        uint64_t window = 0;
        const int n = mp2731_render_diagnostics(json, sizeof json, &window, false);
        CHECK(n > 0 && (size_t)n == strlen(json), "render");
        printf("CHARGER=%s\n", json);
    } else if (!strcmp(c, "off")) {
        /* The fleet as it is (AICO off): five hours, one REG02 read an hour, no write. */
        const int r02 = reg02_reads;
        reads_for(5 * 60 * MIN + 10 * S);
        CHECK(reg00_writes == 0 && reg02_writes == 0, "nothing written");
        CHECK(reg02_reads - r02 == 5 && s_aico.checks == 5 && s_aico.resets == 0, "hourly look (%d)",
              reg02_reads - r02);
        CHECK(reads_of[0x00] == 0 && reads_of[0x14] == 0 && reads_of[0x15] == 0,
              "the off-state look is one REG02 read (REG00 %d, REG14 %d, REG15 %d)",
              reads_of[0x00], reads_of[0x14], reads_of[0x15]);
    } else if (!strcmp(c, "veto")) {
        CHECK(mp2731_set_aico(true, &a) == ESP_OK, "on");
        veto_on = true;                           /* OTA / AMBIT flash */
        reads_for(90 * MIN);
        CHECK(reg00_writes == 0 && s_aico.vetoed == 1, "held for the whole lock, counted once");
        CHECK(mp2731_reset_aico(&a) == ESP_ERR_NOT_ALLOWED, "manual refused under the lock");
        veto_on = false;
        reads_for(5 * S);
        CHECK(reg00_writes == 1, "re-armed once the lock clears");
        /* A manual re-detect in progress (settle window, read mutex free). */
        s_manual_active = true;
        reads_for(61 * MIN);
        CHECK(reg00_writes == 1 && s_aico.vetoed == 2, "held while a manual re-detect runs");
        CHECK(mp2731_reset_aico(&a) == ESP_ERR_TIMEOUT, "manual reset refused while busy");
        s_manual_active = false;
        reads_for(5 * S);
        CHECK(reg00_writes == 2, "then re-armed");
        /* A BG_EN that cannot be proven back at 0. */
        s_bg_en_dirty = true; regs[9] = 0x08; fail_write_reg = 0x09; fail_write_n = 1000;
        reads_for(61 * MIN);
        CHECK(reg00_writes == 2 && s_aico.vetoed == 3, "held over a dirty BG_EN");
        fail_write_n = 0;
        reads_for(10 * S);
        CHECK(!s_bg_en_dirty && reg00_writes == 3, "cleared, then re-armed");
    } else if (!strcmp(c, "failures")) {
        CHECK(mp2731_set_aico(true, &a) == ESP_OK, "on");
        reads_for(60 * MIN - 5 * S);
        fail_write_reg = 0x00; fail_write_n = 1;
        reads_for(10 * S);
        CHECK(s_aico.failed == 1 && s_aico.resets == 0, "write failure counted");
        reads_for(AICO_RETRY_MS - 5 * S);
        CHECK(s_aico.failed == 1 && reg00_writes == 0, "no retry before 5 min");
        corrupt_reg = 0x00;
        reads_for(5 * S);
        CHECK(s_aico.failed == 2 && reg00_writes == 1, "readback mismatch is a failure");
        corrupt_reg = 0xFF; regs[0] = 0x52;
        fail_read_reg = 0x02; fail_read_n = 1;
        reads_for(AICO_RETRY_MS);
        CHECK(s_aico.failed == 3 && reg00_writes == 1, "REG02 unreadable: nothing written");
        reads_for(AICO_RETRY_MS);
        CHECK(s_aico.resets == 1 && reg00_writes == 2, "recovers");
    } else if (!strcmp(c, "manual")) {
        regs[0x14] = 0x04;
        CHECK(mp2731_reset_aico(&a) == ESP_OK && reg00_writes == 1 && regs[0] == 0x52, "rewritten while off");
        CHECK(regs[0x14] == 0x04, "AICO off: the model leaves IIN_DPM alone");
        CHECK(mp2731_set_aico(true, &a) == ESP_OK, "on");
        CHECK(mp2731_reset_aico(&a) == ESP_OK && a.iin_dpm_ma == 1000 && a.resets == 2 && a.has_reset,
              "manual re-arm resets IIN_DPM (%u)", a.iin_dpm_ma);
        reads_for(60 * MIN - 5 * S);
        CHECK(reg00_writes == 2, "and restarts the hour");
        reads_for(10 * S);
        CHECK(reg00_writes == 3, "automatic one an hour later");
        regs[0] = 0xD2;                           /* HIZ set: the byte goes back as read */
        CHECK(mp2731_reset_aico(&a) == ESP_OK && regs[0] == 0xD2, "HIZ/EN_ILIM untouched");
    } else if (!strcmp(c, "status_fail")) {
        /* A verified mutation followed by a failed status read is recorded as
         * done and never replayed (Codex P2). Automatic re-arm first. */
        CHECK(mp2731_set_aico(true, &a) == ESP_OK, "on");
        regs[0x14] = 0x04;
        reads_for(60 * MIN - 5 * S);
        fail_after_write_reg = 0x14; fail_after_write_n = 1;
        reads_for(10 * S);
        CHECK(reg00_writes == 1 && s_aico.resets == 1 && s_aico.failed == 0 && s_aico.has_reset,
              "verified re-arm counted despite the REG14 read failing (resets %u failed %u)",
              s_aico.resets, s_aico.failed);
        reads_for(60 * MIN - 10 * S);
        CHECK(reg00_writes == 1, "not replayed within the hour (%d writes)", reg00_writes);
        reads_for(10 * S);
        CHECK(reg00_writes == 2 && s_aico.resets == 2, "next one on schedule");
        fail_after_write_reg = 0x15; fail_after_write_n = 1;
        reads_for(60 * MIN);
        CHECK(reg00_writes == 3 && s_aico.resets == 3 && s_aico.failed == 0, "same for REG15");
        /* Manual reset: success with status_valid=false. */
        fail_after_write_reg = 0x14; fail_after_write_n = 1;
        CHECK(mp2731_reset_aico(&a) == ESP_OK && !a.status_valid && a.resets == 4 && reg00_writes == 4,
              "manual reset verified, status unread");
        /* Switch: a verified enable followed by a failed REG15 read still
         * starts the learning hour; the next read must not re-arm at once. */
        CHECK(mp2731_set_aico(false, &a) == ESP_OK, "off");
        fail_after_write_reg = 0x15; fail_after_write_n = 1;
        CHECK(mp2731_set_aico(true, &a) == ESP_OK && a.enabled && !a.status_valid && regs[2] == 0xDD,
              "enable verified, status unread");
        CHECK(a.next_check_in_s == 3600, "learning hour started (%u)", a.next_check_in_s);
        const int w = reg00_writes;
        reads_for(60 * MIN - 5 * S);
        CHECK(reg00_writes == w, "no immediate re-arm after the enable");
        /* MQTT shows the unread status as null, never zero. */
        fail_after_write_reg = 0x00; fail_after_write_n = 1;
        request("{\"type\":\"set_aico\",\"id\":\"s\",\"enabled\":true}"); execute();
        cJSON *root = response(true);
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "status_valid")) &&
              cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root, "iin_dpm_ma")) &&
              cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root, "iin_limit_ma")) &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "enabled")), "%s", last_reply);
        cJSON_Delete(root);
        /* A failure BEFORE the write is still a failure, with nothing written. */
        const int before = reg00_writes;
        s_aico.next_ms = uptime_ms();
        fail_read_reg = 0x00; fail_read_n = 1;
        reads_for(5 * S);
        CHECK(s_aico.failed == 1 && reg00_writes == before, "pre-write read failure: failed, nothing written");
    } else if (!strcmp(c, "cli")) {
        char *g[] = {"aico"};
        CHECK(cli_cmd_aico(1, g) == 0 && reg02_writes == 0, "get");
        char *on[] = {"aico", "on"};
        CHECK(cli_cmd_aico(2, on) == 0 && regs[2] == 0xDD, "on");
        char *rs[] = {"aico", "reset"};
        CHECK(cli_cmd_aico(2, rs) == 0 && reg00_writes == 1, "reset");
        char *off[] = {"aico", "off"};
        CHECK(cli_cmd_aico(2, off) == 0 && regs[2] == 0xDC, "off");
        const int calls = cmd_calls;
        char *b1[] = {"aico", "enable"};
        char *b2[] = {"aico", "on", "now"};
        char *b3[] = {"aico", "ON"};
        char *b4[] = {"aico", "1"};
        CHECK(cli_cmd_aico(2, b1) == 1 && cli_cmd_aico(3, b2) == 1 && cli_cmd_aico(2, b3) == 1 &&
              cli_cmd_aico(2, b4) == 1 && cmd_calls == calls, "bad syntax never reaches the charger");
        veto_on = true;
        CHECK(cli_cmd_aico(2, rs) == 1, "reset refused under the lock");
    } else if (!strcmp(c, "mqtt")) {
        request("{\"type\":\"get_aico\",\"id\":\"q-\\\"1\\\"\"}");
        CHECK(job && sessions == 0 && cmd_calls == 0, "deferred: no I2C on the MQTT task");
        execute();
        printf("REPLY=%s\n", last_reply);
        cJSON *root = response(true);
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring, "aico_result") &&
              !strcmp(cJSON_GetObjectItemCaseSensitive(root, "id")->valuestring, "q-\"1\""), "type/id");
        CHECK(!flag(root, "enabled") && num(root, "reg02") == 0xDC && num(root, "iin_limit_ma") == 1000 &&
              num(root, "iin_dpm_ma") == 1000 && !flag(root, "aico_stat") && num(root, "resets") == 0 &&
              cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root, "last_reset_ms")), "fields");
        cJSON_Delete(root);
        request("{\"type\":\"set_aico\",\"id\":\"on\",\"enabled\":true}"); execute();
        root = response(true);
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring, "set_aico_result") &&
              flag(root, "enabled") && num(root, "reg02") == 0xDD && regs[2] == 0xDD, "on");
        cJSON_Delete(root);
        request("{\"type\":\"set_aico\",\"id\":\"on\",\"enabled\":true}"); execute();   /* QoS1 duplicate */
        root = response(true); cJSON_Delete(root);
        CHECK(mp2731_reset_aico(&a) == ESP_OK, "manual");
        request("{\"type\":\"get_aico\",\"id\":\"q\"}"); execute();
        root = response(true);
        CHECK(num(root, "resets") == 1 && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "last_reset_ms")),
              "reset reported");
        cJSON_Delete(root);
        request("{\"type\":\"set_aico\",\"id\":\"off\",\"enabled\":false}"); execute();
        root = response(true);
        CHECK(!flag(root, "enabled") && regs[2] == 0xDC, "off"); cJSON_Delete(root);
        corrupt_reg = 0x02;
        request("{\"type\":\"set_aico\",\"id\":\"bad\",\"enabled\":true}"); execute();
        expect_error("test_error");
    } else if (!strcmp(c, "mqtt_invalid")) {
        const char *bad[] = {"\"true\"", "1", "0", "null", "[]", "{}"};
        for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
            char json[128]; snprintf(json, sizeof json, "{\"type\":\"set_aico\",\"id\":\"e\",\"enabled\":%s}", bad[i]);
            request(json); expect_error("invalid_enabled");
        }
        request("{\"type\":\"set_aico\",\"id\":\"e\"}"); expect_error("invalid_enabled");
        request("{\"type\":\"get_aico\"}"); expect_error("invalid_id");
        request("{\"type\":\"get_aico\",\"id\":\"\"}"); expect_error("invalid_id");
        request("{\"type\":\"get_aico\",\"id\":5}"); expect_error("invalid_id");
        char json[256], id[66]; memset(id, 'x', 65); id[65] = '\0';
        snprintf(json, sizeof json, "{\"type\":\"get_aico\",\"id\":\"%s\"}", id);
        request(json); expect_error("invalid_id");
        CHECK(submits == 0 && sessions == 0 && reg02_writes == 0, "nothing dispatched");
        request("{\"type\":\"get_aico\",\"id\":\"1\"}");
        request("{\"type\":\"set_aico\",\"id\":\"2\",\"enabled\":true}"); expect_error("busy");
        execute();
        CHECK(regs[2] == 0xDC, "the busy refusal wrote nothing");
        reject_submit = 1;
        request("{\"type\":\"get_aico\",\"id\":\"3\"}"); expect_error("queue_unavailable");
        CHECK(!job, "nothing queued");
        reject_submit = 0;
        request("{\"type\":\"get_aico\",\"id\":\"4\"}"); execute();
        cJSON *root = response(true); cJSON_Delete(root);
        const char *others[] = {"{\"type\":\"get_iin_limit\",\"id\":\"x\"}", "{\"type\":\"aico\",\"id\":\"x\"}",
                                "{\"type\":\"reset_aico\",\"id\":\"x\"}", "{\"type\":7}"};
        for (unsigned i = 0; i < sizeof others / sizeof *others; i++) {
            root = cJSON_Parse(others[i]);
            CHECK(!aico_command_handle(root), "%s passes through", others[i]); cJSON_Delete(root);
        }
    } else {
        fprintf(stderr, "unknown case %s\n", c); return 2;
    }
    assert(!locked && !mutex_held && !job);
    puts("OK");
    return 0;
}
