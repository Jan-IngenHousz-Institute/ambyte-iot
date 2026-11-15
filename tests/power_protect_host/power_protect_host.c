/* Host harness for brownout protection (docs/power-protect.md): the PRODUCTION
 * state machine (components/power_protect/power_protect_core.c), its target
 * glue (power_protect.c, #included so a "reboot" can reset its statics while
 * the fake NVS survives), the real power_policy gate that supplies the
 * fresh-external-power evidence, and the MQTT command codec
 * (components/command_router/power_protect_command.c) with the pinned cJSON.
 *
 * Seams faked here: NVS (one blob slot, every write counted, failures
 * injectable), esp_reset_reason, the monotonic and wall clocks (time() via
 * -Wl,--wrap=time), clock_trust's source/suspect, diag's boot counter, the
 * log (every ESP_LOGE counted), the maintenance worker and the allocator.
 *
 * usage: power_protect_host <scenario> | --list   (exit 0 = PASS)
 * Command scenarios also print "REPLY <json>" lines for test_power_protect.py. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../components/power_protect/power_protect.c"
#include "power_policy.h"
#include "power_protect_command.h"

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

/* ── fakes ─────────────────────────────────────────────────────────────── */

static int64_t g_now_us = 1000000;
static time_t  g_utc = 1763152200;          /* 2025-11-14T20:30:00Z = 07:00 ACST 15 Nov */
static const char *g_src = "rtc";
static bool    g_suspect;
static uint32_t g_boot;
static esp_reset_reason_t g_rr = ESP_RST_POWERON;

int64_t esp_timer_get_time(void) { return g_now_us; }
time_t __wrap_time(time_t *t) { if (t) *t = g_utc; return g_utc; }
void clock_trust_get_status(const char **src, bool *suspect) { *src = g_src; *suspect = g_suspect; }
uint32_t diag_boot_count(void) { return g_boot; }
esp_reset_reason_t esp_reset_reason(void) { return g_rr; }
const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }

static int  g_elines, g_wlines;
static char g_last_e[160];
void host_log(char level, const char *tag, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (level == 'E') {
        g_elines++;
        snprintf(g_last_e, sizeof g_last_e, "%s", line);
    } else if (level == 'W') {
        g_wlines++;
    }
    (void)tag;
}

/* One-slot NVS for namespace "pwr_protect", key "state". */
static uint8_t g_blob[256];
static size_t  g_blob_len;       /* 0 = not found */
static int     g_writes;
static bool    g_fail_writes;
static esp_err_t g_open_err = ESP_OK, g_get_err = ESP_OK;   /* injected read-side failures */
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    CHECK(strcmp(ns, "pwr_protect") == 0, "namespace %s", ns);
    if (mode == NVS_READONLY && g_open_err != ESP_OK) return g_open_err;
    *out = 7;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    CHECK(strcmp(key, "state") == 0, "key %s", key);
    if (g_get_err != ESP_OK) return g_get_err;
    if (g_blob_len == 0) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < g_blob_len) return ESP_ERR_INVALID_SIZE;
    memcpy(out, g_blob, g_blob_len);
    *len = g_blob_len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    (void)h;
    CHECK(strcmp(key, "state") == 0 && len <= sizeof g_blob, "key %s len %zu", key, len);
    if (g_fail_writes) return ESP_FAIL;
    memcpy(g_blob, value, len);
    g_blob_len = len;
    g_writes++;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

/* ── helpers ───────────────────────────────────────────────────────────── */

static power_gate_t g_gate;

/* A boot: RAM (statics) gone, NVS kept. valid=false models clock.source hwm. */
static void reboot(esp_reset_reason_t rr, uint32_t boot, bool clock_valid)
{
    s_mtx = NULL;
    memset(&s_core, 0, sizeof s_core);
    s_state_pub = PP_STATE_NORMAL;
    memset(&s_skips, 0, sizeof s_skips);
    power_gate_init(&g_gate);
    g_rr = rr;
    g_boot = boot;
    g_src = clock_valid ? "rtc" : "hwm";
    g_suspect = false;
    g_now_us = 1000000;   /* monotonic restarts at boot */
    power_protect_boot();
}

static void advance_s(int64_t s)
{
    g_now_us += s * 1000000;
    g_utc += (time_t)s;
}

/* One 5-s charger read through the REAL power_policy gate. `vary` = the ADC
 * frame changes (a converting charger); false = a frozen register file. */
static uint16_t g_jitter;
static void read5(power_read_reason_t r, uint16_t sys, uint16_t bat, bool input, bool vary)
{
    power_reading_t p = {
        .battery_mv = bat, .system_mv = sys, .input_mv = input ? 5000 : 0,
        .charge_ma = 0, .input_ma = (uint16_t)(vary ? 100 + (g_jitter++ % 7) : 100),
        .input_present = input,
    };
    power_gate_step(&g_gate, r, r == POWER_READ_OK ? &p : NULL);
    power_protect_on_power_read(r, r == POWER_READ_OK ? &p : NULL, power_gate_flash_ok(&g_gate));
    advance_s(5);
}

static void good_reads(int n, uint16_t sys, uint16_t bat)
{
    for (int i = 0; i < n; i++) read5(POWER_READ_OK, sys, bat, true, true);
}

#define READS_30MIN 360   /* 5-s reads */

static pp_state_t st(void) { return power_protect_state(); }

/* ── scenarios: state machine + glue ───────────────────────────────────── */

static void fresh_boot_no_writes(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    CHECK(st() == PP_STATE_NORMAL && g_writes == 0 && g_elines == 0, "w=%d e=%d", g_writes, g_elines);
    CHECK(!power_protect_blocks_flash() && !power_protect_blocks_ambit(), "probes");
}

static void brownout_enters_no_flash(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    CHECK(st() == PP_STATE_NO_FLASH, "state %d", st());
    CHECK(g_writes == 1 && g_elines == 1, "w=%d e=%d", g_writes, g_elines);
    CHECK(strcmp(g_last_e, "power_protect: normal -> no_flash reason=brownout") == 0, "%s", g_last_e);
    CHECK(power_protect_blocks_flash() && !power_protect_blocks_ambit(), "probes");
    power_protect_status_t s;
    power_protect_get_status(&s);
    CHECK(s.reason == PP_REASON_BROWNOUT && s.since_utc == (int64_t)g_utc && !s.forced &&
          s.brownouts_6h == 1, "status");
}

/* 15 Nov 2026, 14:63:93:39:36:48: brownout boots 2, 3, 4 at 07:00, 07:13, 07:18. */
static void uluru_valid_clock(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    CHECK(st() == PP_STATE_NO_FLASH, "boot 2");
    advance_s(13 * 60);
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_REPEAT_BROWNOUT, "boot 3 escalates");
    CHECK(strcmp(g_last_e, "power_protect: no_flash -> hold reason=repeat_brownout") == 0, "%s", g_last_e);
    advance_s(5 * 60);
    reboot(ESP_RST_BROWNOUT, 4, true);
    CHECK(st() == PP_STATE_HOLD, "boot 4 stays");
    CHECK(g_writes == 3 && g_elines == 2, "w=%d e=%d (history write on boot 4, no transition)", g_writes, g_elines);
    power_protect_status_t s;
    power_protect_get_status(&s);
    CHECK(s.brownouts_6h == 3, "brownouts_6h %u", (unsigned)s.brownouts_6h);
    CHECK(power_protect_blocks_ambit(), "hold sheds ambit");
}

static void uluru_invalid_clock(void)
{
    reboot(ESP_RST_BROWNOUT, 2, false);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.since_utc == 0 && s_core.p.hist[0].utc == 0, "no utc");
    advance_s(13 * 60);
    reboot(ESP_RST_BROWNOUT, 3, false);
    CHECK(st() == PP_STATE_HOLD, "consecutive brownout boots link without a clock");
    power_protect_status_t s;
    power_protect_get_status(&s);
    CHECK(s.brownouts_6h == 2 && s.since_utc == 0, "chain count %u", (unsigned)s.brownouts_6h);
}

static void invalid_clock_not_consecutive(void)
{
    reboot(ESP_RST_BROWNOUT, 2, false);
    reboot(ESP_RST_SW, 3, false);
    reboot(ESP_RST_BROWNOUT, 4, false);
    CHECK(st() == PP_STATE_NO_FLASH, "a boot between: no link without a clock");
    CHECK(g_elines == 1 && g_writes == 2, "e=%d w=%d", g_elines, g_writes);
    power_protect_status_t s;
    power_protect_get_status(&s);
    CHECK(s.brownouts_6h == 1, "only boot 4 is in this boot's chain: %u", (unsigned)s.brownouts_6h);
}

static void valid_clock_far_apart(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    advance_s(7 * 3600);
    reboot(ESP_RST_BROWNOUT, 3, true);   /* consecutive boots, but UTC says 7 h */
    CHECK(st() == PP_STATE_NO_FLASH, "UTC decides when both clocks are valid");
    power_protect_status_t s;
    power_protect_get_status(&s);
    CHECK(s.brownouts_6h == 1, "UTC decides the count too, not the boot chain: %u", (unsigned)s.brownouts_6h);
    /* 5 h 59 min apart with a boot in between: still linked by UTC. */
    reboot(ESP_RST_SW, 4, true);
    advance_s(5 * 3600 + 59 * 60);
    reboot(ESP_RST_BROWNOUT, 5, true);
    CHECK(st() == PP_STATE_HOLD, "UTC links across a non-brownout boot");
}

static void clock_backwards_falls_back(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    g_utc -= 3600;   /* RTC lost an hour: Δ < 0 proves one clock wrong */
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD, "negative delta falls back to consecutive boots");
}

static void clock_sources(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    g_suspect = true;
    CHECK(!clock_now().valid, "suspect rtc is not valid");
    g_src = "sntp";
    CHECK(clock_now().valid, "sntp outranks a sticky boot suspicion");
    g_src = "hwm"; g_suspect = false;
    CHECK(!clock_now().valid, "hwm floor is not the time");
    g_src = "rtc";
    const time_t keep = g_utc;
    g_utc = 1000;
    CHECK(!clock_now().valid, "pre-2024 is not valid");
    g_utc = keep;
    CHECK(clock_now().valid, "rtc, not suspect, plausible");
}

static void exit_after_30min(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    const int w0 = g_writes, e0 = g_elines;
    good_reads(READS_30MIN, 4100, 3900);
    CHECK(st() == PP_STATE_NO_FLASH, "29:55 of evidence is not enough");
    good_reads(2, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.reason == PP_REASON_POWER_RECOVERED, "state %d", st());
    CHECK(g_writes == w0 + 1 && g_elines == e0 + 1, "one write, one line on exit");
    CHECK(strcmp(g_last_e, "power_protect: no_flash -> normal reason=power_recovered") == 0, "%s", g_last_e);
}

/* Every class of bad read restarts the 30-min window. */
static void window_restarts(void)
{
    for (int k = 0; k < 7; k++) {
        g_writes = 0;
        g_blob_len = 0;
        reboot(ESP_RST_BROWNOUT, 2, true);
        good_reads(READS_30MIN - 10, 4100, 3900);           /* 29:10 */
        switch (k) {
        case 0: read5(POWER_READ_NO_INPUT, 0, 0, false, true); break;
        case 1: read5(POWER_READ_IMPLAUSIBLE, 0, 0, false, true); break;
        case 2: read5(POWER_READ_ADC_TIMEOUT, 0, 0, false, true); break;
        case 3: read5(POWER_READ_OK, 3500, 3900, true, true); break;   /* rail below 3.60 V */
        case 4: read5(POWER_READ_OK, 4100, 3650, true, true); break;   /* battery below 3.70 V */
        case 5: read5(POWER_READ_OK, 4100, 3900, false, true); break;  /* valid, input absent */
        case 6: advance_s(90); break;                                   /* nobody read for 90 s */
        }
        good_reads(READS_30MIN - 10, 4100, 3900);
        CHECK(st() == PP_STATE_NO_FLASH, "case %d: window must have restarted", k);
        good_reads(20, 4100, 3900);
        CHECK(st() == PP_STATE_NORMAL, "case %d: a full window after the restart exits", k);
    }
    /* A frozen frame: identical ADC values stop being fresh after the
     * power_policy freshness window, and that restarts the exit window. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(200, 4100, 3900);
    for (int i = 0; i < 20; i++) read5(POWER_READ_OK, 4100, 3900, true, false);
    good_reads(READS_30MIN - 10, 4100, 3900);
    CHECK(st() == PP_STATE_NO_FLASH, "stale frames restart the window");
    good_reads(20, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL, "then exits");
}

static void battery_zero_with_input(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    good_reads(50, 4100, 0);
    CHECK(st() == PP_STATE_NORMAL && g_writes == 0, "battery_v 0 with input never triggers");
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(READS_30MIN + 2, 4100, 0);
    CHECK(st() == PP_STATE_NORMAL, "battery 0 (no battery, rail carries it) qualifies the exit");
}

static void low_voltage_trigger(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    read5(POWER_READ_OK, 3200, 3900, true, true);   /* first frame: no freshness yet */
    CHECK(st() == PP_STATE_NORMAL, "a single frame is not fresh evidence");
    read5(POWER_READ_OK, 3200, 3900, true, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_LOW_VOLTAGE, "rail < 3.30 V");
    CHECK(strcmp(g_last_e, "power_protect: normal -> no_flash reason=low_voltage") == 0, "%s", g_last_e);

    g_blob_len = 0;
    reboot(ESP_RST_POWERON, 5, true);
    good_reads(3, 4100, 3350);
    CHECK(st() == PP_STATE_NO_FLASH, "0 < battery < 3.40 V");

    /* Never on stale, invalid, no_input or input-absent reads. */
    g_blob_len = 0;
    g_writes = 0;
    reboot(ESP_RST_POWERON, 6, true);
    for (int i = 0; i < 30; i++) read5(POWER_READ_OK, 3000, 3000, true, false);   /* frozen */
    read5(POWER_READ_NO_INPUT, 0, 0, false, true);
    read5(POWER_READ_IMPLAUSIBLE, 0, 0, false, true);
    for (int i = 0; i < 5; i++) read5(POWER_READ_OK, 3000, 3000, false, true);    /* on battery */
    CHECK(st() == PP_STATE_NORMAL && g_writes == 0, "no trigger without fresh present evidence");
}

static void hold_never_exits_on_battery(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD, "hold");
    for (int i = 0; i < 4 * READS_30MIN; i++) {
        read5(i % 2 ? POWER_READ_NO_INPUT : POWER_READ_OK, 3900, 3900, false, true);
    }
    CHECK(st() == PP_STATE_HOLD, "2 h on battery: still hold");
    good_reads(READS_30MIN + 2, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL, "external power exits hold");
}

static void forced_never_exits(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_NO_FLASH) == ESP_OK, "force");
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.forced && s_core.p.reason == PP_REASON_FORCED, "forced");
    CHECK(strcmp(g_last_e, "power_protect: normal -> no_flash reason=forced") == 0, "%s", g_last_e);
    good_reads(4 * READS_30MIN, 4100, 3900);
    CHECK(st() == PP_STATE_NO_FLASH, "a forced state never auto-exits");
    const int w = g_writes;
    CHECK(power_protect_force(PP_STATE_NO_FLASH) == ESP_OK && g_writes == w, "re-force: no write");
    /* Survives a reboot; a linked brownout still escalates it, forced stays. */
    reboot(ESP_RST_BROWNOUT, 2, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.forced, "forced survives, first brownout");
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "escalated, still forced");
    good_reads(4 * READS_30MIN, 4100, 3900);
    CHECK(st() == PP_STATE_HOLD, "forced hold never auto-exits");
    CHECK(power_protect_clear() == ESP_OK && st() == PP_STATE_NORMAL && !s_core.p.forced, "clear");
    CHECK(strcmp(g_last_e, "power_protect: hold -> normal reason=cleared") == 0, "%s", g_last_e);
    /* Forcing the current auto state: a relabel, not a transition. */
    reboot(ESP_RST_BROWNOUT, 10, true);
    const int e = g_elines;
    const int64_t since = s_core.p.since_utc;
    advance_s(60);
    CHECK(power_protect_force(PP_STATE_NO_FLASH) == ESP_OK && g_elines == e && s_core.p.forced &&
          s_core.p.since_utc == since, "same-state force keeps since, no E line");
    CHECK(power_protect_force(PP_STATE_NORMAL) == ESP_ERR_INVALID_ARG, "normal is not a force target");
}

static void clear_wipes_history(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    CHECK(power_protect_clear() == ESP_OK && st() == PP_STATE_NORMAL && s_core.p.hist_n == 0, "clear");
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_NO_FLASH, "history gone: a consecutive brownout only enters no_flash");
    CHECK(power_protect_clear() == ESP_OK, "clear");
    const int w = g_writes;
    CHECK(power_protect_clear() == ESP_OK && g_writes == w, "clearing a clean normal writes nothing");
    /* clear also wipes history kept while normal (after a recovery). */
    reboot(ESP_RST_BROWNOUT, 4, true);
    good_reads(READS_30MIN + 2, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.hist_n == 1, "recovered, history kept");
    const int w2 = g_writes;
    CHECK(power_protect_clear() == ESP_OK && g_writes == w2 + 1 && s_core.p.hist_n == 0, "history-only clear writes");
}

/* Recovered (normal) units keep their history: a brownout within 6 h of the
 * previous one goes straight to hold. */
static void recovered_then_repeat(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(READS_30MIN + 2, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL, "recovered");
    advance_s(3600);
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD && strcmp(g_last_e, "power_protect: normal -> hold reason=repeat_brownout") == 0,
          "%s", g_last_e);
}

static void reboot_restarts_window(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(240, 4100, 3900);                   /* 20 min */
    reboot(ESP_RST_SW, 3, true);                   /* RAM window gone */
    CHECK(st() == PP_STATE_NO_FLASH, "persisted across a non-brownout reboot");
    good_reads(200, 4100, 3900);                   /* 16:40 */
    CHECK(st() == PP_STATE_NO_FLASH, "the window restarted at boot");
    good_reads(170, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL, "a full window after boot exits");
}

static void same_boot_recorded_once(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    const int w = g_writes;
    pp_event_t ev = pp_on_boot(&s_core, true, 2, (pp_clock_t){true, (int64_t)g_utc});
    CHECK(!ev.persist && s_core.p.hist_n == 1 && g_writes == w, "idempotent per boot number");
    /* History ring keeps the newest PP_HIST_MAX. */
    for (uint32_t b = 3; b < 3 + 2 * PP_HIST_MAX; b++) reboot(ESP_RST_BROWNOUT, b, false);
    CHECK(s_core.p.hist_n == PP_HIST_MAX && s_core.p.hist[PP_HIST_MAX - 1].boot == 2 + 2 * PP_HIST_MAX,
          "ring");
    power_protect_status_t s;
    power_protect_get_status(&s);
    CHECK(s.brownouts_6h == PP_HIST_MAX, "%u", (unsigned)s.brownouts_6h);
}

static void persistence_roundtrip(void)
{
    pp_persist_t p;
    memset(&p, 0, sizeof p);
    p.state = PP_STATE_HOLD; p.reason = PP_REASON_REPEAT_BROWNOUT; p.forced = true;
    p.since_utc = -1; p.hist_n = 2;
    p.hist[0] = (pp_brownout_t){1763152200, 2};
    p.hist[1] = (pp_brownout_t){0, UINT32_MAX};
    uint8_t b[PP_BLOB_LEN], b2[PP_BLOB_LEN];
    CHECK(pp_encode(&p, b, sizeof b) == PP_BLOB_LEN && PP_BLOB_LEN == 113, "len");
    CHECK(pp_encode(&p, b, PP_BLOB_LEN - 1) == 0, "short buffer refused");
    pp_persist_t q;
    CHECK(pp_decode(&q, b, sizeof b), "decode");
    CHECK(pp_encode(&q, b2, sizeof b2) == PP_BLOB_LEN && memcmp(b, b2, sizeof b) == 0, "round trip");
    CHECK(q.state == PP_STATE_HOLD && q.forced && q.since_utc == -1 && q.hist_n == 2 &&
          q.hist[1].boot == UINT32_MAX && q.hist[2].boot == 0, "fields");
    for (size_t i = 0; i < sizeof b; i++) {     /* any single flipped byte fails */
        memcpy(b2, b, sizeof b);
        b2[i] ^= 0x40;
        CHECK(!pp_decode(&q, b2, sizeof b2), "flip at %zu accepted", i);
    }
    CHECK(!pp_decode(&q, b, sizeof b - 1), "short blob");
    /* Valid CRC, invalid content: range checks still refuse. */
    pp_persist_t bad = p; bad.state = 3;
    pp_encode(&bad, b2, sizeof b2);
    CHECK(!pp_decode(&q, b2, sizeof b2), "state 3");
    bad = p; bad.state = PP_STATE_NORMAL;   /* forced normal is impossible */
    pp_encode(&bad, b2, sizeof b2);
    CHECK(!pp_decode(&q, b2, sizeof b2), "forced normal");
    bad = p; bad.reason = PP_REASON_COUNT;
    pp_encode(&bad, b2, sizeof b2);
    CHECK(!pp_decode(&q, b2, sizeof b2), "reason");
    b2[0] = 2;
    CHECK(!pp_decode(&q, b2, sizeof b2), "version");

    /* Through the glue: what a brownout boot wrote is what the next boot reads. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 2, true);
    reboot(ESP_RST_BROWNOUT, 3, true);
    const pp_persist_t before = s_core.p;
    reboot(ESP_RST_POWERON, 4, true);
    CHECK(memcmp(&before.hist, &s_core.p.hist, sizeof before.hist) == 0 && s_core.p.state == before.state &&
          s_core.p.since_utc == before.since_utc && s_core.p.reason == before.reason, "glue round trip");
    /* A corrupted blob starts at no_flash (state_unreadable), logged; this
     * boot's brownout is still recorded, and that write repairs the blob. */
    g_blob[5] ^= 1;
    const int warn = g_wlines, w = g_writes;
    reboot(ESP_RST_BROWNOUT, 5, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_STATE_UNREADABLE &&
          s_core.p.hist_n == 1 && g_wlines > warn, "corrupt -> no_flash");
    CHECK(g_writes == w + 1 && !s_core.persist_fault, "the brownout record repaired the blob");
}

/* Finding 2 of the PR #103 review: an unreadable persisted state must not
 * resume full load on a NON-brownout reboot. */
static void unreadable_state_is_protected(void)
{
    /* forced hold, one stored byte flipped, a watchdog (sw) reboot */
    reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "force");
    g_blob[20] ^= 0x10;
    const int w = g_writes;
    reboot(ESP_RST_SW, 2, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_STATE_UNREADABLE && !s_core.p.forced,
          "state %d reason %d", st(), s_core.p.reason);
    CHECK(s_core.persist_fault && g_writes == w, "flagged, nothing written at boot");
    char json[PP_JSON_CAP];
    CHECK(power_protect_render_json(json, sizeof json) > 0 && strstr(json, "\"persist_fault\":true") &&
          strstr(json, "\"reason\":\"state_unreadable\""), "%s", json);
    printf("JSON_UNREADABLE %s\n", json);
    power_protect_status_t s;
    power_protect_get_status(&s);
    CHECK(s.persist_fault && s.state == PP_STATE_NO_FLASH, "status");
    /* Still unreadable on the next boot (not repaired yet): still protected. */
    reboot(ESP_RST_INT_WDT, 3, true);
    CHECK(st() == PP_STATE_NO_FLASH && g_writes == w, "no write until a transition");
    /* Exits through the normal 30-min window; that transition repairs NVS. */
    good_reads(READS_30MIN + 2, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL && g_writes == w + 1 && !s_core.persist_fault, "recovered + repaired");
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_NORMAL && !s_core.persist_fault, "repaired blob reads back");
    /* ... or through clear. */
    g_blob[30] ^= 1;
    reboot(ESP_RST_SW, 5, true);
    CHECK(st() == PP_STATE_NO_FLASH && power_protect_clear() == ESP_OK && st() == PP_STATE_NORMAL &&
          !s_core.persist_fault, "clear repairs");
    /* A stored blob of the wrong size is unreadable too. */
    g_blob_len = PP_BLOB_LEN - 1;
    reboot(ESP_RST_SW, 6, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.persist_fault, "short blob");
}

static void read_failures_are_protected(void)
{
    /* A previously protected unit whose NVS read fails, open or get. */
    reboot(ESP_RST_BROWNOUT, 2, true);
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD, "hold stored");
    g_get_err = ESP_FAIL;
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_STATE_UNREADABLE && s_core.persist_fault,
          "get_blob failure");
    g_get_err = ESP_OK;
    g_open_err = ESP_FAIL;
    reboot(ESP_RST_POWERON, 5, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.persist_fault, "nvs_open failure");
    /* Namespace missing (first install) is ABSENT, not unreadable. */
    g_open_err = ESP_ERR_NVS_NOT_FOUND;
    reboot(ESP_RST_POWERON, 6, true);
    CHECK(st() == PP_STATE_NORMAL && !s_core.persist_fault, "absent namespace");
    g_open_err = ESP_OK;
    g_blob_len = 0;
    reboot(ESP_RST_POWERON, 7, true);
    CHECK(st() == PP_STATE_NORMAL && !s_core.persist_fault, "absent key");
    /* A failed write raises the flag; the next good one clears it. */
    g_fail_writes = true;
    CHECK(power_protect_force(PP_STATE_NO_FLASH) != ESP_OK && s_core.persist_fault, "write failure flagged");
    g_fail_writes = false;
    CHECK(power_protect_clear() == ESP_OK && !s_core.persist_fault, "next write clears it");
}

/* Finding 3 of the PR #103 review: no clock AND no boot counter. */
static void unknown_boot_counter(void)
{
    reboot(ESP_RST_BROWNOUT, 0, false);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_BROWNOUT, "first");
    reboot(ESP_RST_BROWNOUT, 0, false);
    CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_BROWNOUT_UNORDERED && s_core.p.hist_n == 2,
          "state %d hist %u", st(), (unsigned)s_core.p.hist_n);
    CHECK(strcmp(g_last_e, "power_protect: no_flash -> hold reason=brownout_unordered") == 0, "%s", g_last_e);
    power_protect_status_t s;
    power_protect_get_status(&s);
    CHECK(s.brownouts_6h == 2, "unknown-age entries count: %u", (unsigned)s.brownouts_6h);
    /* Counter recovered (0 then nonzero), and lost (nonzero then 0). */
    for (int k = 0; k < 2; k++) {
        g_blob_len = 0;
        reboot(ESP_RST_BROWNOUT, k == 0 ? 0 : 7, false);
        CHECK(st() == PP_STATE_NO_FLASH, "k=%d first", k);
        reboot(ESP_RST_BROWNOUT, k == 0 ? 5 : 0, false);
        CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_BROWNOUT_UNORDERED, "k=%d escalates", k);
        power_protect_get_status(&s);
        CHECK(s.brownouts_6h == 2, "k=%d count %u", k, (unsigned)s.brownouts_6h);
    }
    /* A valid clock still orders brownouts with a counter of 0: UTC decides. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 0, true);
    advance_s(7 * 3600);
    reboot(ESP_RST_BROWNOUT, 0, true);
    CHECK(st() == PP_STATE_NO_FLASH, "7 h apart by UTC: not linked");
    advance_s(3600);
    reboot(ESP_RST_BROWNOUT, 0, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_REPEAT_BROWNOUT, "1 h apart: linked");
    /* Unordered only escalates a PROTECTED unit: after a recovery it is a
     * fresh episode (no_flash), and no periodic write ever happens. */
    good_reads(READS_30MIN + 2, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL, "recovered");
    g_src = "hwm";
    const int w = g_writes;
    good_reads(500, 4100, 3900);
    CHECK(g_writes == w, "no periodic writes");
    reboot(ESP_RST_BROWNOUT, 0, false);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_BROWNOUT, "normal -> no_flash only");
}

static void no_writes_on_reads_or_reports(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    const int w = g_writes;
    char json[PP_JSON_CAP];
    power_protect_status_t s;
    for (int i = 0; i < 300; i++) {
        read5(i % 3 ? POWER_READ_OK : POWER_READ_NO_INPUT, 4100, 3900, i % 3 != 0, true);
        CHECK(power_protect_render_json(json, sizeof json) > 0, "render");
        power_protect_get_status(&s);
        power_protect_note_skip(POWER_PROTECT_SKIP_FLASH_RUN);
    }
    CHECK(g_writes == w, "reads, heartbeats, status and skips never write NVS (%d)", g_writes - w);
    reboot(ESP_RST_POWERON, 3, true);
    good_reads(2000, 4100, 3900);
    CHECK(g_writes == w + 1, "exactly one write on the exit transition (%d)", g_writes - w);
}

static void render_json(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    power_protect_note_skip(POWER_PROTECT_SKIP_FLASH_RUN);
    power_protect_note_skip(POWER_PROTECT_SKIP_FLASH_RUN);
    power_protect_note_skip(POWER_PROTECT_SKIP_AMBIT_RUN);
    power_protect_note_skip(POWER_PROTECT_SKIP_OTA);
    char json[PP_JSON_CAP];
    CHECK(power_protect_render_json(json, sizeof json) > 0, "render");
    char want[PP_JSON_CAP];
    snprintf(want, sizeof want, "{\"state\":\"no_flash\",\"reason\":\"brownout\",\"since_utc\":%lld,"
             "\"forced\":false,\"persist_fault\":false,\"brownouts_6h\":1,"
             "\"skipped\":{\"flash_runs\":2,\"ambit_runs\":1,\"ota\":1}}",
             (long long)g_utc);
    CHECK(strcmp(json, want) == 0, "%s", json);
    printf("JSON %s\n", json);
    /* Unknown since -> null; worst case fits PP_JSON_CAP; too small -> -1, empty. */
    pp_persist_t p;
    memset(&p, 0, sizeof p);
    CHECK(pp_render_json(json, sizeof json, &p, false, 0, NULL) > 0 && strstr(json, "\"since_utc\":null"), "%s", json);
    /* Worst case: the longest state and reason names, every number at its widest. */
    size_t longest = 0;
    pp_reason_t widest = PP_REASON_NONE;
    for (int r = 0; r < PP_REASON_COUNT; r++) {
        if (strlen(pp_reason_name((pp_reason_t)r)) > longest) {
            longest = strlen(pp_reason_name((pp_reason_t)r));
            widest = (pp_reason_t)r;
        }
    }
    p.state = PP_STATE_NO_FLASH; p.reason = (uint8_t)widest; p.forced = true;
    p.since_utc = INT64_MIN;
    const pp_skips_t max = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const int n = pp_render_json(json, sizeof json, &p, true, UINT32_MAX, &max);
    CHECK(n > 0 && (size_t)n < PP_JSON_CAP, "worst case %d", n);
    printf("WORST %d %s\n", n, json);
    CHECK(pp_render_json(json, 40, &p, false, 0, NULL) == -1 && json[0] == '\0', "no partial object");
}

/* ── scenarios: the MQTT command codec, wired to the real glue ─────────── */

static void (*g_job)(void *);
static void *g_job_arg;
static bool g_queue_full, g_maint_held;
static int g_maint_depth;
static bool submit(void (*run)(void *), void *arg)
{
    if (g_queue_full || g_job != NULL) return false;
    g_job = run; g_job_arg = arg;
    return true;
}
static bool maint_begin(void) { if (g_maint_held) return false; g_maint_depth++; return true; }
static void maint_end(void) { g_maint_depth--; }
static void reply(const char *json) { printf("REPLY %s\n", json); }
static void run_job(void) { CHECK(g_job != NULL, "no job queued"); void (*f)(void *) = g_job; g_job = NULL; f(g_job_arg); }

static int g_alloc_n, g_fail_at;
static void *hook_malloc(size_t n) { if (g_fail_at && ++g_alloc_n == g_fail_at) return NULL; return malloc(n); }

static void cmd_init(void)
{
    static cJSON_Hooks hooks = { hook_malloc, free };
    cJSON_InitHooks(&hooks);
    const power_protect_command_config_t c = {
        .reply = reply, .submit = submit, .maintenance_begin = maint_begin, .maintenance_end = maint_end,
        .get_status = power_protect_get_status, .force = power_protect_force,
        .clear = power_protect_clear, .state_now = power_protect_state,
    };
    power_protect_command_init(&c);
}

static void cmd(const char *json, bool run)
{
    cJSON *root = cJSON_Parse(json);
    CHECK(root != NULL, "parse %s", json);
    CHECK(power_protect_command_handle(root), "handled");
    cJSON_Delete(root);
    if (run) run_job();
}

static void cmd_flow(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    cmd_init();
    cJSON *other = cJSON_Parse("{\"type\":\"ping\",\"id\":\"x\"}");
    CHECK(!power_protect_command_handle(other), "other types pass through");
    cJSON_Delete(other);
    cmd("{\"type\":\"power_protect\",\"id\":\"s1\",\"action\":\"status\"}", true);
    CHECK(g_maint_depth == 0, "status takes no maintenance lock");
    const int w = g_writes;
    cmd("{\"type\":\"power_protect\",\"id\":\"f1\",\"action\":\"force\",\"state\":\"hold\"}", true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced && g_writes == w + 1 && g_maint_depth == 0, "force");
    cmd("{\"type\":\"power_protect\",\"id\":\"c1\",\"action\":\"clear\"}", true);
    CHECK(st() == PP_STATE_NORMAL && !s_core.p.forced && s_core.p.hist_n == 0, "clear");
    cmd("{\"type\":\"power_protect\",\"id\":\"s2\",\"action\":\"status\",\"state\":null}", true);
}

static void cmd_refusals(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    cmd_init();
    const char *bad[] = {
        "{\"type\":\"power_protect\",\"action\":\"status\"}",
        "{\"type\":\"power_protect\",\"id\":\"\",\"action\":\"status\"}",
        "{\"type\":\"power_protect\",\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"action\":\"status\"}",
        "{\"type\":\"power_protect\",\"id\":7,\"action\":\"status\"}",
        "{\"type\":\"power_protect\",\"id\":\"q\\\"1\",\"action\":\"reboot\"}",
        "{\"type\":\"power_protect\",\"id\":\"a2\"}",
        "{\"type\":\"power_protect\",\"id\":\"a3\",\"action\":5}",
        "{\"type\":\"power_protect\",\"id\":\"f2\",\"action\":\"force\"}",
        "{\"type\":\"power_protect\",\"id\":\"f3\",\"action\":\"force\",\"state\":\"normal\"}",
        "{\"type\":\"power_protect\",\"id\":\"f4\",\"action\":\"force\",\"state\":\"HOLD\"}",
        "{\"type\":\"power_protect\",\"id\":\"c2\",\"action\":\"clear\",\"state\":\"hold\"}",
        "{\"type\":\"power_protect\",\"id\":\"s3\",\"action\":\"status\",\"state\":\"no_flash\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cmd(bad[i], false);
        CHECK(g_job == NULL, "case %zu must not queue", i);
    }
    CHECK(st() == PP_STATE_NORMAL && g_writes == 0, "nothing changed");
    /* busy: one op at a time; queue_unavailable: the worker refused it. */
    cmd("{\"type\":\"power_protect\",\"id\":\"b1\",\"action\":\"status\"}", false);
    cmd("{\"type\":\"power_protect\",\"id\":\"b2\",\"action\":\"status\"}", false);
    run_job();
    g_queue_full = true;
    cmd("{\"type\":\"power_protect\",\"id\":\"q1\",\"action\":\"status\"}", false);
    g_queue_full = false;
    cmd("{\"type\":\"power_protect\",\"id\":\"q2\",\"action\":\"status\"}", true);   /* busy flag released */
    /* maintenance held: clear/force refused, nothing applied. */
    g_maint_held = true;
    cmd("{\"type\":\"power_protect\",\"id\":\"m1\",\"action\":\"force\",\"state\":\"no_flash\"}", true);
    CHECK(st() == PP_STATE_NORMAL, "not applied while maintenance holds the lock");
    g_maint_held = false;
}

static void cmd_persist_failed(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    cmd_init();
    g_fail_writes = true;
    cmd("{\"type\":\"power_protect\",\"id\":\"p1\",\"action\":\"force\",\"state\":\"no_flash\"}", true);
    g_fail_writes = false;
    CHECK(st() == PP_STATE_NO_FLASH, "applied in RAM even when NVS refused");
    reboot(ESP_RST_POWERON, 2, true);
    CHECK(st() == PP_STATE_NORMAL, "... and therefore gone after a reboot");
}

static void cmd_with_alloc_fail(const char *json, int at)
{
    cJSON *root = cJSON_Parse(json);
    CHECK(root != NULL, "parse");
    g_alloc_n = 0;
    g_fail_at = at;          /* 1 = the request copy, 2.. = the success reply */
    CHECK(power_protect_command_handle(root), "handled");
    if (g_job != NULL) run_job();
    g_fail_at = 0;
    cJSON_Delete(root);
}

static void cmd_no_mem(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    cmd_init();
    /* Every single allocation failure: the reply is either a complete success
     * or an allocation-free refusal (queue_unavailable / no_mem), never a
     * partial ok:true; the busy flag is always released. */
    for (int at = 1; at <= 40; at++) {
        cmd_with_alloc_fail("{\"type\":\"power_protect\",\"id\":\"n1\",\"action\":\"status\"}", at);
        CHECK(g_job == NULL, "nothing left queued");
    }
    cmd("{\"type\":\"power_protect\",\"id\":\"n3\",\"action\":\"status\"}", true);
}

/* ── main ──────────────────────────────────────────────────────────────── */

typedef struct { const char *name; void (*fn)(void); } scenario_t;
static const scenario_t k_scenarios[] = {
    {"fresh_boot_no_writes", fresh_boot_no_writes},
    {"brownout_enters_no_flash", brownout_enters_no_flash},
    {"uluru_valid_clock", uluru_valid_clock},
    {"uluru_invalid_clock", uluru_invalid_clock},
    {"invalid_clock_not_consecutive", invalid_clock_not_consecutive},
    {"valid_clock_far_apart", valid_clock_far_apart},
    {"clock_backwards_falls_back", clock_backwards_falls_back},
    {"clock_sources", clock_sources},
    {"exit_after_30min", exit_after_30min},
    {"window_restarts", window_restarts},
    {"battery_zero_with_input", battery_zero_with_input},
    {"low_voltage_trigger", low_voltage_trigger},
    {"hold_never_exits_on_battery", hold_never_exits_on_battery},
    {"forced_never_exits", forced_never_exits},
    {"clear_wipes_history", clear_wipes_history},
    {"recovered_then_repeat", recovered_then_repeat},
    {"reboot_restarts_window", reboot_restarts_window},
    {"same_boot_recorded_once", same_boot_recorded_once},
    {"persistence_roundtrip", persistence_roundtrip},
    {"no_writes_on_reads_or_reports", no_writes_on_reads_or_reports},
    {"render_json", render_json},
    {"unreadable_state_is_protected", unreadable_state_is_protected},
    {"read_failures_are_protected", read_failures_are_protected},
    {"unknown_boot_counter", unknown_boot_counter},
    {"cmd_flow", cmd_flow},
    {"cmd_refusals", cmd_refusals},
    {"cmd_persist_failed", cmd_persist_failed},
    {"cmd_no_mem", cmd_no_mem},
};

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <scenario>|--list\n", argv[0]); return 2; }
    for (size_t i = 0; i < sizeof k_scenarios / sizeof k_scenarios[0]; i++) {
        if (strcmp(argv[1], "--list") == 0) { puts(k_scenarios[i].name); continue; }
        if (strcmp(argv[1], k_scenarios[i].name) == 0) {
            k_scenarios[i].fn();
            printf("PASS %s\n", k_scenarios[i].name);
            return 0;
        }
    }
    if (strcmp(argv[1], "--list") == 0) return 0;
    fprintf(stderr, "unknown scenario %s\n", argv[1]);
    return 2;
}
