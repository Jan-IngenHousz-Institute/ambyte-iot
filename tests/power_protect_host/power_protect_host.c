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

/* The recovery scenarios (accumulated exit, battery-only probe, idle battery
 * 0) also compile against the PREVIOUS core (origin/main a13fbfd) to prove
 * they fail there: only the state machine's observable behaviour is checked
 * when the recovery API is absent. */
#ifdef PP_PROBE_FAILS_MAX
#define HAVE_RECOVERY 1
#else
#define HAVE_RECOVERY 0
#endif
/* Likewise the durability round (commit-first, RTC copy, durable low), so
 * its regressions also build against the round-0 head b4103f5. */
#ifdef PP_RELAX_RETRY_MS
#define HAVE_DURABILITY 1
#else
#define HAVE_DURABILITY 0
#endif

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
#if HAVE_DURABILITY
    /* RTC_NOINIT memory survives resets without power loss; after a power
     * loss it holds garbage. */
    if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT || rr == ESP_RST_PWR_GLITCH) {
        memset(&s_unwritten, 0xA5, sizeof s_unwritten);
    }
#endif
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
#define H (3600)

/* Reads carrying no evidence at the power guard's 15-s cadence: NO_INPUT is
 * the battery-only unit (live status: no input), I2C_ERROR / UNAVAILABLE a
 * charger that does not answer / is not fitted. */
static void quiet_reads(power_read_reason_t r, int64_t seconds)
{
    for (int64_t t = 0; t < seconds; t += 15) {
        power_gate_step(&g_gate, r, NULL);
        power_protect_on_power_read(r, NULL, power_gate_flash_ok(&g_gate));
        advance_s(15);
    }
}

#if HAVE_RECOVERY
static pp_recovery_t rec(void)
{
    power_protect_status_t s;
    power_protect_get_status(&s);
    return s.recovery;
}
#endif

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

/* Reads that carry no evidence PAUSE the accumulation (it keeps what it had);
 * a fresh read with input below the exit thresholds RESETS it. */
static void window_restarts(void)
{
    for (int k = 0; k < 8; k++) {
        g_writes = 0;
        g_blob_len = 0;
        reboot(ESP_RST_BROWNOUT, 2, true);
        good_reads(READS_30MIN - 10, 4100, 3900);           /* 29:05 collected */
        bool resets = false;
        switch (k) {
        case 0: read5(POWER_READ_NO_INPUT, 0, 0, false, true); break;
        case 1: read5(POWER_READ_IMPLAUSIBLE, 0, 0, false, true); break;
        case 2: read5(POWER_READ_ADC_TIMEOUT, 0, 0, false, true); break;
        case 3: read5(POWER_READ_OK, 3500, 3900, true, true); resets = true; break;   /* rail below 3.60 V */
        case 4: read5(POWER_READ_OK, 4100, 3650, true, true); resets = true; break;   /* battery below 3.70 V */
        case 5: read5(POWER_READ_OK, 4100, 3900, false, true); break;  /* valid, input absent */
        case 6: advance_s(90); break;                                   /* nobody read for 90 s */
        case 7: read5(POWER_READ_I2C_ERROR, 0, 0, false, true); break;
        }
        if (!resets) {
            /* The interval around the pause earns nothing, nor do the reads
             * before power_policy calls the frames fresh again. */
            good_reads(4, 4100, 3900);
            CHECK(st() == PP_STATE_NO_FLASH, "case %d: under 30 min", k);
            good_reads(12, 4100, 3900);                  /* well under a minute more */
            CHECK(st() == PP_STATE_NORMAL, "case %d: a pause keeps what was collected", k);
            continue;
        }
        good_reads(READS_30MIN - 10, 4100, 3900);
        CHECK(st() == PP_STATE_NO_FLASH, "case %d: a low read must have reset the evidence", k);
        good_reads(20, 4100, 3900);
        CHECK(st() == PP_STATE_NORMAL, "case %d: a full 30 min after the reset exits", k);
    }
    /* A frozen frame: identical ADC values stop being fresh after the
     * power_policy freshness window. Stale is not evidence: it pauses. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(200, 4100, 3900);
    for (int i = 0; i < 20; i++) read5(POWER_READ_OK, 4100, 3900, true, false);
    CHECK(st() == PP_STATE_NO_FLASH, "stale frames are not evidence");
    good_reads(READS_30MIN - 150, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL, "then the rest of the 30 min exits");
}

static void battery_zero_with_input(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    good_reads(50, 4100, 0);
    CHECK(st() == PP_STATE_NORMAL && g_writes == 0, "battery_v 0 with a healthy rail never triggers");
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(READS_30MIN + 2, 4100, 0);
    CHECK(st() == PP_STATE_NORMAL, "battery 0 with a rail above the battery threshold qualifies the exit");
}

/* Uluru 18 Nov, 10:BD:A3:63:FD:20: the charger idles beside a live panel
 * (VIN 7-8 V, not charging), battery_v reads 0 and system_v IS the battery
 * through the BATFET. It left protection on a 3.68 V rail and browned out the
 * same day. The rail must meet the BATTERY threshold (3.70 V). */
static void idle_battery_zero(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(2 * READS_30MIN, 3650, 0);               /* an hour at 3.65 V */
    CHECK(st() == PP_STATE_NO_FLASH, "3.65 V rail with battery_v 0 must not qualify");
    good_reads(READS_30MIN + 2, 3750, 0);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.reason == PP_REASON_POWER_RECOVERED, "3.75 V does");
    /* And it is a reset, not a pause: 25 min at 3.75 V, one idle 3.65 V read,
     * 25 min more is not enough. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 3, true);
    good_reads(300, 3750, 0);
    good_reads(1, 3650, 0);
    good_reads(300, 3750, 0);
    CHECK(st() == PP_STATE_NO_FLASH, "an idle low rail resets the evidence");
}

/* Entry is unchanged from the first release: battery_v 0 never triggers on
 * its own. Many Uluru units idle on 3.3-3.6 V rails; entering no_flash on a
 * 3.35 V idle rail would cut their Fv/Fm data. Only system_v < 3.30 V does. */
static void idle_rail_entry_unchanged(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    good_reads(60, 3350, 0);
    CHECK(st() == PP_STATE_NORMAL && g_writes == 0, "a 3.35 V idle rail with battery_v 0 must not enter");
    good_reads(3, 3250, 0);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_LOW_VOLTAGE, "a rail below 3.30 V enters");
#if HAVE_RECOVERY
    /* The normal probation is failed by the same entry rule: a 3.35 V idle
     * rail does not fail it. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 2, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL, "probing");
    good_reads(30, 3350, 0);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "3.35 V idle rail: probation goes on");
#endif
}

/* ── scenarios: recovery on solar and on battery (Uluru 17-20 Nov) ─────── */

/* A weak panel: 12-min charging bursts, a collapse to the 4.3 V floor (input
 * absent) and 20 min of cloud (no_input) between them. The continuous rule
 * never exited; accumulated evidence does on the third burst. */
static void intermittent_solar_recovers(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    const int e0 = g_elines, w0 = g_writes;
    for (int burst = 0; burst < 3; burst++) {
        good_reads(144, 4000, 3850);                       /* 12 min: 11:55 of evidence */
        if (burst == 2) break;
        CHECK(st() == PP_STATE_NO_FLASH, "burst %d: not 30 min yet", burst);
        for (int i = 0; i < 12; i++) read5(POWER_READ_OK, 3900, 3850, false, true);   /* collapsed */
        quiet_reads(POWER_READ_NO_INPUT, 20 * 60);                                     /* cloud */
    }
    CHECK(st() == PP_STATE_NORMAL && s_core.p.reason == PP_REASON_POWER_RECOVERED, "3 x 12 min recovers: %d", st());
    CHECK(g_elines == e0 + 1 && g_writes == w0 + 1, "one line, one write (e=%d w=%d)", g_elines - e0, g_writes - w0);
    CHECK(strcmp(g_last_e, "power_protect: no_flash -> normal reason=power_recovered") == 0, "%s", g_last_e);

    /* Start-stop charging: 80-s spells, 60 s collapsed between them. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 3, true);
    for (int spell = 0; spell < 30; spell++) {
        if (spell == 20) CHECK(st() == PP_STATE_NO_FLASH, "20 spells (< 25 min) are not enough");
        good_reads(16, 4000, 3850);
        for (int i = 0; i < 12; i++) read5(POWER_READ_OK, 3900, 3850, false, true);
    }
    CHECK(st() == PP_STATE_NORMAL, "30 x 80-s spells recover");
}

/* A fresh read with input below the exit thresholds throws everything away. */
static void low_read_resets_accumulation(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(121, 4100, 3900);                           /* 10 min */
    quiet_reads(POWER_READ_NO_INPUT, 600);
    good_reads(121, 4100, 3900);                           /* 20 min collected */
#if HAVE_RECOVERY
    const uint32_t got = rec().qualifying_s;   /* 1200 s less the reads power_policy needs to call it fresh */
    CHECK(got >= 1180 && got <= 1200, "collected %u", (unsigned)got);
    power_protect_status_t s;
    power_protect_get_status(&s);
    CHECK(s.exit_in_s == (int64_t)(1800 - got), "exit_in_s %lld", (long long)s.exit_in_s);
#endif
    read5(POWER_READ_OK, 4100, 3600, true, true);          /* battery 3.60 V: low */
#if HAVE_RECOVERY
    CHECK(rec().qualifying_s == 0, "reset to %u", (unsigned)rec().qualifying_s);
#endif
    good_reads(181, 4100, 3900);                           /* 15 min */
    quiet_reads(POWER_READ_NO_INPUT, 600);
    good_reads(169, 4100, 3900);                           /* 29 min since the reset */
    CHECK(st() == PP_STATE_NO_FLASH, "the 20 min before the low read must not count");
    good_reads(13, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL, "30 min after the reset exits");
}

/* Evidence counts inside a 12-h horizon only. While the charger does not
 * answer (I2C errors) there is no probe, so only the accumulation decides. */
static void old_evidence_expires(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(241, 4100, 3900);                           /* 20 min */
    quiet_reads(POWER_READ_I2C_ERROR, 10 * H);
    good_reads(125, 4100, 3900);                           /* +10 min, 10.5 h later */
    CHECK(st() == PP_STATE_NORMAL, "control: evidence 10.5 h apart adds up");

    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 3, true);
#if HAVE_RECOVERY
    /* Once the old evidence has expired, the first read from a live charger
     * (not yet fresh) would probe: take the probe out to isolate the horizon. */
    s_core.p.probe_fails = PP_PROBE_FAILS_MAX;
#endif
    good_reads(241, 4100, 3900);
    quiet_reads(POWER_READ_I2C_ERROR, 12 * H + 30 * 60);
    good_reads(125, 4100, 3900);
    CHECK(st() == PP_STATE_NO_FLASH, "20 min from 12.5 h ago expired");
    good_reads(240, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL, "fresh 30 min exits");
}

/* 14:63:93:37:A0:C4, 18 Nov: browned out at 11:11, panel delivering nothing
 * (no_input on 186 of 187 heartbeats), no_flash all day. Probe after 6 h quiet,
 * probation 24 h across the nightly reboot, then passed. */
static void battery_probe_recovers(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H - 60);
    CHECK(st() == PP_STATE_NO_FLASH, "5:59 quiet: no probe yet");
#if HAVE_RECOVERY
    pp_recovery_t r = rec();
    CHECK(r.probe == PP_PROBE_WAITING && r.next_probe_s > 0 && r.next_probe_s <= 60 && r.backoff_level == 0,
          "waiting %d next %lld", r.probe, (long long)r.next_probe_s);
#endif
    const int w0 = g_writes;
    (void)w0;
    quiet_reads(POWER_READ_NO_INPUT, 120);
    CHECK(st() == PP_STATE_NORMAL, "probe: no_flash -> normal (state %d)", st());
#if HAVE_RECOVERY
    CHECK(s_core.p.reason == PP_REASON_PROBE && s_core.p.probation == PP_PROBATION_NORMAL && g_writes == w0 + 1,
          "reason %d probation %d w %d", s_core.p.reason, s_core.p.probation, g_writes - w0);
    CHECK(strcmp(g_last_e, "power_protect: no_flash -> normal reason=probe") == 0, "%s", g_last_e);
    CHECK(rec().probe == PP_PROBE_PROBATION && rec().next_probe_s == -1, "probation");
#endif
    CHECK(!power_protect_blocks_flash(), "saturating flashes run again on probation");
    quiet_reads(POWER_READ_NO_INPUT, 10 * H);
    reboot(ESP_RST_SW, 3, true);                           /* the nightly maintenance reboot */
    CHECK(st() == PP_STATE_NORMAL, "probation survives a reboot");
    quiet_reads(POWER_READ_NO_INPUT, 13 * H);
#if HAVE_RECOVERY
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL, "23 h: still on probation");
    const int e = g_elines, w = g_writes;
    quiet_reads(POWER_READ_NO_INPUT, H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NONE && st() == PP_STATE_NORMAL, "24 h quiet: passed");
    CHECK(g_elines == e + 1 && g_writes == w + 1, "one line, one write (e=%d w=%d)", g_elines - e, g_writes - w);
    CHECK(strcmp(g_last_e, "power_protect: normal probation passed reason=probe_passed backoff_level=0") == 0,
          "%s", g_last_e);
    CHECK(rec().probe == PP_PROBE_OFF, "off");
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NONE, "passed survives a reboot");
    /* A later brownout is a new episode (no_flash), not a failed probe. */
    advance_s(7 * H);
    reboot(ESP_RST_BROWNOUT, 5, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_BROWNOUT && s_core.p.probe_fails == 0, "new episode");
#endif
}

#if HAVE_RECOVERY
/* Probe from no_flash to normal and fail it with a brownout. */
static void probe_and_fail(uint32_t *boot, int64_t wait_s, pp_state_t from)
{
    CHECK(st() == from, "from %d (state %d)", from, st());
    quiet_reads(POWER_READ_NO_INPUT, wait_s - 120);
    CHECK(st() == from, "not before %lld s (state %d)", (long long)wait_s, st());
    quiet_reads(POWER_READ_NO_INPUT, 240);
    CHECK(s_core.p.probation != PP_PROBATION_NONE && s_core.p.reason == PP_REASON_PROBE, "probed after %lld s",
          (long long)wait_s);
    advance_s(H);
    reboot(ESP_RST_BROWNOUT, ++*boot, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_PROBE_FAILED && s_core.p.probation == 0,
          "failed probe escalates to hold (state %d reason %d)", st(), s_core.p.reason);
}
#endif

/* A brownout on probation fails the probe: hold, and the next probe waits
 * 6 -> 12 -> 24 h (capped), across reboots; 6 failures exhaust it. */
static void probation_brownout_backoff(void)
{
    uint32_t boot = 2;
    reboot(ESP_RST_BROWNOUT, boot, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NORMAL, "probed");
    advance_s(H);
    reboot(ESP_RST_BROWNOUT, ++boot, true);
    CHECK(st() == PP_STATE_HOLD, "a brownout on probation -> hold");
#if HAVE_RECOVERY
    CHECK(s_core.p.reason == PP_REASON_PROBE_FAILED && s_core.p.probe_fails == 1, "level 1");
    CHECK(strcmp(g_last_e, "power_protect: normal -> hold reason=probe_failed") == 0, "%s", g_last_e);
    reboot(ESP_RST_SW, ++boot, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails == 1, "the level survives a reboot");
    quiet_reads(POWER_READ_NO_INPUT, 15);
    CHECK(rec().backoff_level == 1 && rec().next_probe_s > 11 * H, "next %lld", (long long)rec().next_probe_s);
    /* hold steps down to no_flash after 12 h; fail it (level 2), then 24 h. */
    probe_and_fail(&boot, 12 * H, PP_STATE_HOLD);
    CHECK(s_core.p.probe_fails == 2, "level 2");
    reboot(ESP_RST_SW, ++boot, true);
    probe_and_fail(&boot, 24 * H, PP_STATE_HOLD);
    CHECK(s_core.p.probe_fails == 3, "level 3, wait capped at 24 h");
    probe_and_fail(&boot, 24 * H, PP_STATE_HOLD);
    probe_and_fail(&boot, 24 * H, PP_STATE_HOLD);
    probe_and_fail(&boot, 24 * H, PP_STATE_HOLD);
    CHECK(s_core.p.probe_fails == PP_PROBE_FAILS_MAX, "level %u", (unsigned)s_core.p.probe_fails);
    CHECK(rec().probe == PP_PROBE_EXHAUSTED && rec().next_probe_s == -1, "exhausted");
    const int w = g_writes;
    quiet_reads(POWER_READ_NO_INPUT, 72 * H);
    CHECK(st() == PP_STATE_HOLD && g_writes == w, "no further probe, no writes");
    reboot(ESP_RST_SW, ++boot, true);
    CHECK(rec().probe == PP_PROBE_EXHAUSTED, "exhausted survives a reboot");
    /* A qualifying exit resets the backoff. */
    good_reads(READS_30MIN + 2, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probe_fails == 0, "qualifying exit resets the level");
    /* ... and so does clear. */
    reboot(ESP_RST_BROWNOUT, ++boot, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    advance_s(H);
    reboot(ESP_RST_BROWNOUT, ++boot, true);
    CHECK(s_core.p.probe_fails == 1, "level 1 again");
    CHECK(power_protect_clear() == ESP_OK && s_core.p.probe_fails == 0 && st() == PP_STATE_NORMAL, "clear resets");
#endif
}

/* hold never jumps to normal: no_flash first (steady-state work), then, after
 * 24 h of no_flash probation without a brownout, normal on probation. */
static void hold_steps_down_first(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    advance_s(13 * 60);
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD, "hold");
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NO_FLASH, "hold -> no_flash first (state %d)", st());
    CHECK(power_protect_blocks_flash() && !power_protect_blocks_ambit(), "steady-state work only");
#if HAVE_RECOVERY
    CHECK(s_core.p.probation == PP_PROBATION_NO_FLASH, "on probation");
    CHECK(strcmp(g_last_e, "power_protect: hold -> no_flash reason=probe") == 0, "%s", g_last_e);
#endif
    quiet_reads(POWER_READ_NO_INPUT, 24 * H - 600);
    CHECK(st() == PP_STATE_NO_FLASH, "not normal during the no_flash probation");
    quiet_reads(POWER_READ_NO_INPUT, 900);
    CHECK(st() == PP_STATE_NORMAL, "then normal (state %d)", st());
#if HAVE_RECOVERY
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL && s_core.p.reason == PP_REASON_PROBE, "normal on probation");
    /* The no_flash probation can fail on a fresh low read: back to hold. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 10, true);
    reboot(ESP_RST_BROWNOUT, 11, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NO_FLASH, "probing");
    good_reads(3, 4100, 3600);                    /* below exit, above entry: not a failure */
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probation == PP_PROBATION_NO_FLASH, "3.60 V does not fail it");
    good_reads(3, 4100, 3350);                    /* below the entry threshold */
    CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_PROBE_FAILED && s_core.p.probe_fails == 1,
          "a low read fails the no_flash probation");
    /* And the normal probation: back to no_flash. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 20, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL, "probing");
    good_reads(3, 3250, 3900);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_PROBE_FAILED && s_core.p.probe_fails == 1,
          "a low read fails the normal probation");
#endif
}

static void forced_never_probes(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_NO_FLASH) == ESP_OK, "force");
    quiet_reads(POWER_READ_NO_INPUT, 72 * H);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.forced, "forced no_flash: 72 h on battery, no probe");
#if HAVE_RECOVERY
    CHECK(rec().probe == PP_PROBE_OFF && rec().next_probe_s == -1, "probe off");
#endif
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "force hold");
    quiet_reads(POWER_READ_NO_INPUT, 72 * H);
    CHECK(st() == PP_STATE_HOLD, "forced hold: no probe");
    /* Forcing a unit on probation ends the probation. */
    CHECK(power_protect_clear() == ESP_OK, "clear");
    reboot(ESP_RST_BROWNOUT, 2, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NORMAL, "probed");
    CHECK(power_protect_force(PP_STATE_NO_FLASH) == ESP_OK && st() == PP_STATE_NO_FLASH, "forced");
#if HAVE_RECOVERY
    CHECK(s_core.p.probation == PP_PROBATION_NONE, "force ends the probation");
#endif
    quiet_reads(POWER_READ_NO_INPUT, 72 * H);
    CHECK(st() == PP_STATE_NO_FLASH, "no probe while forced");
}

/* No charger monitor: no probe (fail-closed, as before). */
static void no_monitor_no_probe(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    advance_s(48 * H);                              /* nothing reads the charger at all */
#if HAVE_RECOVERY
    CHECK(rec().probe == PP_PROBE_NO_MONITOR && rec().next_probe_s == -1, "no_monitor");
#endif
    quiet_reads(POWER_READ_UNAVAILABLE, 48 * H);
    CHECK(st() == PP_STATE_NO_FLASH, "no charger fitted: no probe");
    quiet_reads(POWER_READ_I2C_ERROR, 48 * H);
    CHECK(st() == PP_STATE_NO_FLASH, "charger not answering: no probe");
#if HAVE_RECOVERY
    CHECK(rec().probe == PP_PROBE_NO_MONITOR, "still no_monitor");
#endif
    quiet_reads(POWER_READ_NO_INPUT, 30);           /* a live "no input" answer */
    CHECK(st() == PP_STATE_NORMAL, "a live monitor saying no input probes");
}

/* A fresh low read restarts the quiet period; the probe waits 6 h from it. */
static void probe_waits_after_low_read(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    quiet_reads(POWER_READ_NO_INPUT, 5 * H);
    good_reads(3, 4100, 3600);                      /* fresh, input, battery 3.60 V */
    quiet_reads(POWER_READ_NO_INPUT, 6 * H - 120);
    CHECK(st() == PP_STATE_NO_FLASH, "11 h after entry, under 6 h after the low read");
    quiet_reads(POWER_READ_NO_INPUT, 240);
    CHECK(st() == PP_STATE_NORMAL, "6 h after the low read: probe");
}

/* Without a valid clock the quiet period and the probation count uptime: any
 * reboot restarts them (conservative). */
static void probe_uptime_without_clock(void)
{
    reboot(ESP_RST_BROWNOUT, 2, false);
    quiet_reads(POWER_READ_NO_INPUT, 5 * H);
    reboot(ESP_RST_SW, 3, false);
    quiet_reads(POWER_READ_NO_INPUT, 5 * H);
    CHECK(st() == PP_STATE_NO_FLASH, "10 h, but only 5 h of uptime");
    quiet_reads(POWER_READ_NO_INPUT, H + 60);
    CHECK(st() == PP_STATE_NORMAL, "6 h of uptime: probe");
#if HAVE_RECOVERY
    quiet_reads(POWER_READ_NO_INPUT, 20 * H);
    reboot(ESP_RST_SW, 4, false);
    quiet_reads(POWER_READ_NO_INPUT, 20 * H);
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL, "probation restarted at the reboot");
    quiet_reads(POWER_READ_NO_INPUT, 4 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NONE, "24 h of uptime: passed");
    /* A probation that fails on a low read: the next quiet period counts from
     * the failure, not from boot. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 10, false);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL, "probing");
    quiet_reads(POWER_READ_NO_INPUT, 4 * H);
    good_reads(3, 3250, 3900);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1, "failed on a low read");
    quiet_reads(POWER_READ_NO_INPUT, 12 * H - 120);
    CHECK(st() == PP_STATE_NO_FLASH, "12 h from the failure, not from boot");
    quiet_reads(POWER_READ_NO_INPUT, 240);
    CHECK(st() == PP_STATE_NORMAL, "then the next probe");
#endif
}

/* Qualifying evidence inside the horizon blocks the probe: the accumulation
 * decides; once it has expired the probe may run. */
static void evidence_blocks_probe(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(121, 4100, 3900);                    /* 10 min of evidence */
    quiet_reads(POWER_READ_NO_INPUT, 11 * H);
    CHECK(st() == PP_STATE_NO_FLASH, "evidence 11 h old still blocks the probe");
#if HAVE_RECOVERY
    CHECK(rec().probe == PP_PROBE_WAITING && rec().next_probe_s > 0 && rec().next_probe_s <= H,
          "next %lld", (long long)rec().next_probe_s);
#endif
    quiet_reads(POWER_READ_NO_INPUT, H + 60);
    CHECK(st() == PP_STATE_NORMAL, "expired evidence: the probe runs");
}

#if HAVE_RECOVERY
static uint32_t crc32_v1(const uint8_t *b, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
#endif

/* The first release's 113-B v1 blob reads back unchanged (an upgrade never
 * turns a stored state into "unreadable"); the next write is v2. */
static void blob_v1_upgrade(void)
{
#if HAVE_RECOVERY
    pp_persist_t p;
    memset(&p, 0, sizeof p);
    p.state = PP_STATE_HOLD; p.reason = PP_REASON_REPEAT_BROWNOUT; p.since_utc = (int64_t)g_utc;
    p.hist_n = 2; p.hist[0] = (pp_brownout_t){(int64_t)g_utc - 600, 2}; p.hist[1] = (pp_brownout_t){(int64_t)g_utc, 3};
    uint8_t v2[PP_BLOB_LEN];
    CHECK(pp_encode(&p, v2, sizeof v2) == PP_BLOB_LEN && PP_BLOB_LEN == 124 && PP_BLOB_V1_LEN == 113, "lens");
    /* v1 = v2 without the two probe bytes, version 1, its own CRC. */
    memcpy(g_blob, v2, PP_BLOB_V1_LEN - 4);
    g_blob[0] = 1;
    const uint32_t crc = crc32_v1(g_blob, PP_BLOB_V1_LEN - 4);
    for (int i = 0; i < 4; i++) g_blob[PP_BLOB_V1_LEN - 4 + i] = (uint8_t)(crc >> (8 * i));
    g_blob_len = PP_BLOB_V1_LEN;
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_REPEAT_BROWNOUT && !s_core.persist_fault &&
          s_core.p.hist_n == 2 && s_core.p.probation == 0 && s_core.p.probe_fails == 0, "v1 read back");
    CHECK(g_writes == 0, "nothing rewritten at boot");
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK && g_blob_len == PP_BLOB_LEN && g_blob[0] == 2, "v2 written");
    /* v2 range checks: a probation must be automatic and in its step's state. */
    pp_persist_t q;
    pp_persist_t bad = p; bad.state = PP_STATE_NORMAL; bad.probation = PP_PROBATION_NO_FLASH;
    pp_encode(&bad, v2, sizeof v2);
    CHECK(!pp_decode(&q, v2, sizeof v2), "no_flash probation in normal");
    bad = p; bad.state = PP_STATE_NO_FLASH; bad.forced = true; bad.probation = PP_PROBATION_NO_FLASH;
    pp_encode(&bad, v2, sizeof v2);
    CHECK(!pp_decode(&q, v2, sizeof v2), "forced probation");
    bad = p; bad.probation = 3;
    pp_encode(&bad, v2, sizeof v2);
    CHECK(!pp_decode(&q, v2, sizeof v2), "probation 3");
    bad = p; bad.probe_fails = PP_PROBE_FAILS_MAX + 1;
    pp_encode(&bad, v2, sizeof v2);
    CHECK(!pp_decode(&q, v2, sizeof v2), "fails");
#if HAVE_DURABILITY
    bad = p; bad.has_low = true; bad.low_utc = 1763152200;
    pp_encode(&bad, v2, sizeof v2);
    CHECK(pp_decode(&q, v2, sizeof v2) && q.has_low && q.low_utc == 1763152200, "durable low round trip");
    v2[PP_BLOB_LEN - 4 - 9] = 0;      /* has_low 0 with a low_utc: refused (CRC fixed below) */
    {
        const uint32_t crc = crc32_v1(v2, PP_BLOB_LEN - 4);
        for (int i = 0; i < 4; i++) v2[PP_BLOB_LEN - 4 + i] = (uint8_t)(crc >> (8 * i));
    }
    CHECK(!pp_decode(&q, v2, sizeof v2), "low_utc without has_low");
#endif
    bad = p; bad.state = PP_STATE_NORMAL; bad.probation = PP_PROBATION_NORMAL; bad.probe_fails = 3;
    pp_encode(&bad, v2, sizeof v2);
    CHECK(pp_decode(&q, v2, sizeof v2) && q.probation == PP_PROBATION_NORMAL && q.probe_fails == 3, "valid probation");
    /* A v1 version byte on a v2-length blob (and the reverse) is refused. */
    pp_encode(&p, v2, sizeof v2);
    v2[0] = 1;
    CHECK(!pp_decode(&q, v2, sizeof v2), "v1 byte, v2 length");
    /* An unreadable blob assumes the longest backoff wait (level cap). */
    g_blob[7] ^= 1;
    reboot(ESP_RST_SW, 5, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == PP_PROBE_LEVEL_CAP && s_core.persist_fault, "unreadable");
#endif
}

/* ── scenarios: durability of the probe (review of PR #113, round 1) ───── */

#if HAVE_RECOVERY
/* [P1] The probe step's commit fails: nothing relaxes, before or after a
 * brownout or a software reset; the next try waits an hour. */
static void probe_step_commit_fault(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    const int w = g_writes;
    g_fail_writes = true;
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probation == PP_PROBATION_NONE && s_core.persist_fault,
          "an uncommitted probe step never takes effect (state %d)", st());
    CHECK(g_elines == 1, "no transition line for an uncommitted step (%d)", g_elines);
    CHECK(rec().probe == PP_PROBE_WAITING && rec().next_probe_s > 55 * 60 && rec().next_probe_s <= 60 * 60,
          "retry in an hour: %lld", (long long)rec().next_probe_s);
    quiet_reads(POWER_READ_NO_INPUT, 50 * 60);
    CHECK(st() == PP_STATE_NO_FLASH, "no retry inside the hour");
    /* Brownouts with NVS still refusing: never relaxed, so never a cycle. */
    for (uint32_t i = 0; i < 8; i++) {
        reboot(ESP_RST_BROWNOUT, 3 + i, true);
        quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
        CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probation == PP_PROBATION_NONE, "cycle %u: stays no_flash", i);
    }
    CHECK(g_writes == w, "every write refused");
    /* A software reset: still protected; with NVS back the probe commits. */
    reboot(ESP_RST_SW, 20, true);
    CHECK(st() == PP_STATE_NO_FLASH, "a software reset relaxes nothing");
    g_fail_writes = false;
    quiet_reads(POWER_READ_NO_INPUT, 60);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL && !s_core.persist_fault &&
          g_writes == w + 1, "committed probe (w %d)", g_writes - w);
    reboot(ESP_RST_SW, 21, true);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "on record");
    /* Within one boot: NVS comes back during the hold, the retry commits. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 30, true);
    g_fail_writes = true;
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NO_FLASH, "refused");
    g_fail_writes = false;
    quiet_reads(POWER_READ_NO_INPUT, 30 * 60);
    CHECK(st() == PP_STATE_NO_FLASH, "the hold runs its hour");
    quiet_reads(POWER_READ_NO_INPUT, 31 * 60);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "retried and committed");
}

/* [P1] The probe FAILURE's write fails. The protective state holds in RAM;
 * a software reset brings it back from the RTC copy; a power loss counts the
 * stored probation as failed. Never full load from the stored probation. */
static void failure_commit_fault(void)
{
    static const esp_reset_reason_t after[] = { ESP_RST_SW, ESP_RST_TASK_WDT, ESP_RST_BROWNOUT, ESP_RST_POWERON,
                                                ESP_RST_PWR_GLITCH };
    for (unsigned k = 0; k < sizeof after / sizeof after[0]; k++) {
        g_blob_len = 0;
        g_fail_writes = false;
        reboot(ESP_RST_BROWNOUT, 2, true);
        quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
        CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "k=%u probation on record", k);
        g_fail_writes = true;
        advance_s(H);
        reboot(ESP_RST_BROWNOUT, 3, true);
        CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails == 1 && s_core.persist_fault, "k=%u failure in RAM", k);
        advance_s(2 * H);
        reboot(after[k], 4, true);
        CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails >= 1 && s_core.p.probation == PP_PROBATION_NONE,
              "k=%u: reset %d brought back state %d fails %u", k, after[k], st(), (unsigned)s_core.p.probe_fails);
        /* NVS back: the next write puts it on record. */
        g_fail_writes = false;
        CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK && !s_core.persist_fault, "k=%u repaired", k);
        reboot(ESP_RST_SW, 5, true);
        CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails >= 1, "k=%u on record", k);
    }
    /* No unwritten state: a reset without power loss keeps the probation
     * (the nightly reboot must not fail it); a power-on fails it. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 10, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    reboot(ESP_RST_INT_WDT, 11, true);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "a watchdog keeps the probation");
    reboot(ESP_RST_POWERON, 12, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_PROBE_FAILED && s_core.p.probe_fails == 1 &&
          s_core.p.hist_n == 1, "power-on on probation: failed (no brownout recorded)");
    CHECK(strcmp(g_last_e, "power_protect: normal -> hold reason=probe_failed") == 0, "%s", g_last_e);
    /* Garbage in RTC memory after a power-on never decodes. */
    g_blob_len = 0;
    reboot(ESP_RST_POWERON, 20, true);
    CHECK(st() == PP_STATE_NORMAL && !s_core.persist_fault, "garbage is not a state");
}

/* [P1] The accumulated exit and the probation pass commit first too. */
static void relaxation_commit_fault(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    g_fail_writes = true;
    good_reads(READS_30MIN + 2, 4100, 3900);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.persist_fault, "uncommitted exit: still protected");
    good_reads(10, 4100, 3900);
    g_fail_writes = false;
    good_reads(300, 4100, 3900);                   /* 25 min more, inside the hour */
    CHECK(st() == PP_STATE_NO_FLASH, "no retry inside the hour");
    good_reads(450, 4100, 3900);                   /* past the hour */
    CHECK(st() == PP_STATE_NORMAL && s_core.p.reason == PP_REASON_POWER_RECOVERED && !s_core.persist_fault,
          "exit committed after the hold");
    /* The probation pass. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 3, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL, "probing");
    g_fail_writes = true;
    quiet_reads(POWER_READ_NO_INPUT, 24 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL && s_core.persist_fault, "an unrecorded pass is not a pass");
    g_fail_writes = false;
    quiet_reads(POWER_READ_NO_INPUT, H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NONE && !s_core.persist_fault, "passed once on record");
}
#endif

/* [P2] A fresh low read vetoes the probe for a full quiet period, and a
 * reboot does not erase that: low read -> software reset -> input lost, for
 * no_flash and hold, with and without a valid clock. */
static void low_veto_survives_reboot(void)
{
    for (int k = 0; k < 4; k++) {
        const bool hold = k & 1, clock = k < 2;
        g_blob_len = 0;
        reboot(ESP_RST_BROWNOUT, 2, clock);
        if (hold) {
            advance_s(600);
            reboot(ESP_RST_BROWNOUT, 3, clock);
        }
        const pp_state_t from = hold ? PP_STATE_HOLD : PP_STATE_NO_FLASH;
        CHECK(st() == from, "k=%d setup", k);
        quiet_reads(POWER_READ_NO_INPUT, 6 * H - 60);
        good_reads(3, 4100, 3350);                 /* fresh, input, battery 3.35 V */
        quiet_reads(POWER_READ_NO_INPUT, 90);
        CHECK(st() == from, "k=%d: the low read holds the probe", k);
        reboot(ESP_RST_SW, 10, clock);             /* the nightly / no-PUBACK reboot */
        quiet_reads(POWER_READ_NO_INPUT, 6 * H - 120);
        CHECK(st() == from, "k=%d: 6 h after the low read, across the reboot: still %d (state %d)", k, from, st());
        if (clock) {
            /* The stored low counts an hour later (it may be that stale). */
            quiet_reads(POWER_READ_NO_INPUT, H);
            CHECK(st() == from, "k=%d: the refresh margin", k);
        }
        quiet_reads(POWER_READ_NO_INPUT, 300);
        CHECK(st() != from, "k=%d: then the probe (state %d)", k, st());
    }
}

/* The durable low costs at most one write per hour of low reads. */
static void low_writes_bounded(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    const int w = g_writes;
    for (int i = 0; i < 10 * 720; i++) read5(POWER_READ_OK, 4100, 3500, true, true);   /* 10 h, every 5 s */
    CHECK(st() == PP_STATE_NO_FLASH, "low all along");
    CHECK(g_writes - w >= 10 && g_writes - w <= 11, "one write an hour: %d", g_writes - w);
    /* Without a valid clock: one write (undated), then none until it can be dated. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 3, false);
    const int w2 = g_writes;
    for (int i = 0; i < 2 * 720; i++) read5(POWER_READ_OK, 4100, 3500, true, true);
    CHECK(g_writes == w2 + 1, "undated: one write (%d)", g_writes - w2);
    g_src = "rtc";
    read5(POWER_READ_OK, 4100, 3500, true, true);
    CHECK(g_writes == w2 + 2, "dated once the clock is valid (%d)", g_writes - w2);
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
    CHECK(pp_encode(&p, b, sizeof b) == PP_BLOB_LEN && PP_BLOB_LEN == 124, "len");
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
    b2[0] = 3;
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
#if HAVE_RECOVERY   /* the heartbeat object of this core only */
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
             "\"qualifying_s\":0,\"probe\":\"no_monitor\",\"next_probe_s\":null,\"backoff_level\":0,"
             "\"skipped\":{\"flash_runs\":2,\"ambit_runs\":1,\"ota\":1}}",
             (long long)g_utc);
    CHECK(strcmp(json, want) == 0, "%s", json);
    printf("JSON %s\n", json);
    /* Unknown since -> null; worst case fits PP_JSON_CAP; too small -> -1, empty. */
    pp_persist_t p;
    memset(&p, 0, sizeof p);
    CHECK(pp_render_json(json, sizeof json, &p, false, 0, NULL, NULL) > 0 && strstr(json, "\"since_utc\":null") &&
          strstr(json, "\"next_probe_s\":null"), "%s", json);
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
    const pp_recovery_t wide = { UINT32_MAX, PP_PROBE_NO_MONITOR, INT64_MAX, UINT8_MAX };
    const int n = pp_render_json(json, sizeof json, &p, true, UINT32_MAX, &max, &wide);
    CHECK(n > 0 && (size_t)n < PP_JSON_CAP, "worst case %d", n);
    printf("WORST %d %s\n", n, json);
    CHECK(pp_render_json(json, 40, &p, false, 0, NULL, NULL) == -1 && json[0] == '\0', "no partial object");
    /* A unit on probation, through the glue. */
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 3, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(power_protect_render_json(json, sizeof json) > 0, "render");
    printf("JSON_PROBATION %s\n", json);
    g_blob_len = 0;
    reboot(ESP_RST_BROWNOUT, 4, true);
    good_reads(61, 4100, 3900);
    quiet_reads(POWER_READ_NO_INPUT, H);
    CHECK(power_protect_render_json(json, sizeof json) > 0, "render");
    printf("JSON_WAITING %s\n", json);
#endif
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
    for (int at = 1; at <= 60; at++) {
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
    {"idle_battery_zero", idle_battery_zero},
    {"idle_rail_entry_unchanged", idle_rail_entry_unchanged},
    {"intermittent_solar_recovers", intermittent_solar_recovers},
    {"low_read_resets_accumulation", low_read_resets_accumulation},
    {"old_evidence_expires", old_evidence_expires},
    {"battery_probe_recovers", battery_probe_recovers},
    {"probation_brownout_backoff", probation_brownout_backoff},
    {"hold_steps_down_first", hold_steps_down_first},
    {"forced_never_probes", forced_never_probes},
    {"no_monitor_no_probe", no_monitor_no_probe},
    {"probe_waits_after_low_read", probe_waits_after_low_read},
    {"probe_uptime_without_clock", probe_uptime_without_clock},
    {"evidence_blocks_probe", evidence_blocks_probe},
    {"blob_v1_upgrade", blob_v1_upgrade},
#if HAVE_RECOVERY
    {"probe_step_commit_fault", probe_step_commit_fault},
    {"failure_commit_fault", failure_commit_fault},
    {"relaxation_commit_fault", relaxation_commit_fault},
#endif
    {"low_veto_survives_reboot", low_veto_survives_reboot},
    {"low_writes_bounded", low_writes_bounded},
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
