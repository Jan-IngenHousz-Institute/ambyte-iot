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
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Reset injection (declared before the glue: its PP_TEST_POINT uses it).
 * g_reset_at: at the NVS boundary, 1 = the unit resets as nvs_set_blob starts
 * (nothing stored), 2 = right after nvs_commit (stored, but the glue never
 * updates RAM / the RTC mirror). g_rtc_reset_at: inside rtc_store, 1 = before
 * the target slot is invalidated, 2 = between invalidating and rewriting it,
 * 3 = after rewriting, before arming it. The test longjmps out of the glue
 * and calls reboot(), which resets every static the glue owns. */
static jmp_buf g_reset_jump;
static int     g_reset_at;
static int     g_rtc_reset_at;
#define PP_TEST_POINT(n) do { if (g_rtc_reset_at == (n)) { g_rtc_reset_at = 0; longjmp(g_reset_jump, 1); } } while (0)
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
/* Round 4: POWER_PROTECT_ERR_UNCONFIRMED (fence confirmation). */
#ifdef POWER_PROTECT_ERR_UNCONFIRMED
#define HAVE_EPOCH_R4 1
#else
#define HAVE_EPOCH_R4 0
#endif
#ifdef PP_FENCE_RUN
#define HAVE_R5 1
#else
#define HAVE_R5 0
#endif
/* And the generation-ordered record of review round 2 (s_rtc mirror). */
#ifdef PP_RTC_MAGIC
#define HAVE_GEN 1
#else
#define HAVE_GEN 0
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

#ifdef PP_NATIVE_NVS
/* -DPP_NATIVE_NVS: the NVS calls below go to the PINNED ESP-IDF NVS on an
 * in-memory partition (nvs_native/nvs_native.cpp), not to the fake. */
void native_format(void);
void native_reinit(void);
void native_fault(int write_at, int read_at);
void native_fault2(int write_a, int write_b);
int  native_open(int readwrite);
int  native_get_blob(void *out, size_t *len);
int  native_set_blob(const void *src, size_t len);
int  native_get_u32(uint32_t *out);
int  native_set_u32(uint32_t value);
#define NATIVE(call) return (call)
#else
#define NATIVE(call) ((void)0)
#endif

/* One-slot NVS for namespace "pwr_protect", key "state". */
static uint8_t g_blob[256];
static size_t  g_blob_len;       /* 0 = not found */
static int     g_writes;
static bool    g_fail_writes;
static esp_err_t g_open_err = ESP_OK, g_get_err = ESP_OK;   /* injected read-side failures */
/* NVS writes that store the blob and THEN report an error, as the pinned
 * IDF's Storage::writeItem can (ESP_ERR_NVS_REMOVE_FAILED erasing the old
 * copy). Counts down: each such write decrements it. */
static int     g_write_then_fail;
/* What the pinned IDF does right after such a write (native repro of PR #113
 * round 4): 1 = reads return NOT_FOUND (Page::alterEntryState invalidated the
 * page for this boot), 2 = reads return the OLD record; either way the NEW
 * one is read after a re-init (reboot() clears it). */
static int     g_hide_after_write;
/* After such a landed-then-error write, NVS refuses every further write
 * (the fence too) until reboot. */
static bool    g_refuse_after_landed;
static bool    g_landed_then_failed;
static int     g_hidden;
static uint8_t g_old_blob[256];
static size_t  g_old_len;
/* The last NVS state writes, oldest first (ordering checks). */
#define WRITE_LOG 8
static uint8_t g_wlog[WRITE_LOG][256];
static int     g_wlog_n;
/* The epoch key (u32). 0 = absent. */
static uint32_t g_epoch;
static int      g_epoch_writes;
static bool     g_fail_epoch_writes;   /* epoch writes refused, state writes not */
static bool     g_epoch_lands_error;   /* the epoch write lands, then reports an error */
static esp_err_t g_epoch_get_err = ESP_OK;   /* only the epoch read fails */
/* Every pp_encode from the glue passes through here (-Wl,--wrap=pp_encode):
 * g_encode_reset_n > 0 resets the unit AT the n-th call from now. */
static int g_encode_reset_n;
size_t __real_pp_encode(const pp_persist_t *p, uint8_t *buf, size_t cap);
size_t __wrap_pp_encode(const pp_persist_t *p, uint8_t *buf, size_t cap)
{
    if (g_encode_reset_n > 0 && --g_encode_reset_n == 0) longjmp(g_reset_jump, 1);
    return __real_pp_encode(p, buf, cap);
}
uint32_t esp_random(void) { static uint32_t x = 0x2545F491u; x = x * 1664525u + 1013904223u; return x; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    CHECK(strcmp(ns, "pwr_protect") == 0, "namespace %s", ns);
    *out = 7;
    NATIVE(native_open(mode == NVS_READWRITE));
    if (mode == NVS_READONLY && g_open_err != ESP_OK) return g_open_err;
    *out = 7;
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    (void)h;
    CHECK(strcmp(key, "epoch") == 0, "key %s", key);
    NATIVE(native_get_u32(out));
    if (g_get_err != ESP_OK) return g_get_err;
    if (g_epoch_get_err != ESP_OK) return g_epoch_get_err;
    if (g_epoch == 0) return ESP_ERR_NVS_NOT_FOUND;
    *out = g_epoch;
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    (void)h;
    CHECK(strcmp(key, "epoch") == 0, "key %s", key);
    NATIVE(native_set_u32(value));
    if (g_fail_writes || g_fail_epoch_writes) return ESP_FAIL;
    g_epoch = value;
    g_epoch_writes++;
    return g_epoch_lands_error ? 0x1108 : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    CHECK(strcmp(key, "state") == 0, "key %s", key);
    NATIVE(native_get_blob(out, len));
    if (g_get_err != ESP_OK) return g_get_err;
    if (g_hidden == 1) return ESP_ERR_NVS_NOT_FOUND;
    if (g_hidden == 2) {
        if (g_old_len == 0) return ESP_ERR_NVS_NOT_FOUND;
        if (*len < g_old_len) return ESP_ERR_INVALID_SIZE;
        memcpy(out, g_old_blob, g_old_len);
        *len = g_old_len;
        return ESP_OK;
    }
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
    NATIVE(native_set_blob(value, len));
    if (g_reset_at == 1) { g_reset_at = 0; longjmp(g_reset_jump, 1); }
    if (g_fail_writes || (g_refuse_after_landed && g_landed_then_failed)) return ESP_FAIL;
    if (g_hide_after_write && g_write_then_fail > 0) {
        memcpy(g_old_blob, g_blob, g_blob_len);
        g_old_len = g_blob_len;
        g_hidden = g_hide_after_write;
    }
    memcpy(g_blob, value, len);
    g_blob_len = len;
    g_writes++;
    memmove(g_wlog[0], g_wlog[1], sizeof g_wlog[0] * (WRITE_LOG - 1));
    memcpy(g_wlog[WRITE_LOG - 1], value, len);
    g_wlog_n++;
    if (g_write_then_fail > 0) {
        g_write_then_fail--;
        g_landed_then_failed = true;
        return 0x1108;   /* ESP_ERR_NVS_REMOVE_FAILED: written, old copy not erased */
    }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    if (g_reset_at == 2) { g_reset_at = 0; longjmp(g_reset_jump, 1); }
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }

/* ── helpers ───────────────────────────────────────────────────────────── */

static power_gate_t g_gate;

/* A new (or wiped) device: no record, no epoch. */
static void fresh_nvs(void)
{
    g_blob_len = 0;
    g_epoch = 0;
}

/* A boot: RAM (statics) gone, NVS kept. valid=false models clock.source hwm. */
static void reboot(esp_reset_reason_t rr, uint32_t boot, bool clock_valid)
{
    s_mtx = NULL;
    memset(&s_core, 0, sizeof s_core);
    g_hidden = 0;   /* a re-init reads what NVS really holds */
    g_landed_then_failed = false;
#ifdef PP_NATIVE_NVS
    native_reinit();
#endif
#if HAVE_GEN
    /* RTC_NOINIT memory survives resets without power loss; after a power
     * loss it holds garbage. */
    if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT || rr == ESP_RST_PWR_GLITCH) {
        memset(&s_rtc, 0xA5, sizeof s_rtc);
    }
#elif HAVE_DURABILITY
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
    /* A first install writes its record (gen 1) and then the epoch, once:
     * from then on an epoch without a record means the state was lost. */
    CHECK(st() == PP_STATE_NORMAL && g_writes == 1 && g_epoch_writes == 1 && g_elines == 0, "w=%d e=%d",
          g_writes, g_elines);
    CHECK(!power_protect_blocks_flash() && !power_protect_blocks_ambit(), "probes");
    reboot(ESP_RST_SW, 2, true);
    reboot(ESP_RST_POWERON, 3, true);
    CHECK(st() == PP_STATE_NORMAL && g_writes == 1 && g_epoch_writes == 1, "nothing more at later boots (w=%d)",
          g_writes);
}

static void brownout_enters_no_flash(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    CHECK(st() == PP_STATE_NO_FLASH, "state %d", st());
    CHECK(g_writes == 2 && g_elines == 1, "w=%d e=%d (first record + brownout)", g_writes, g_elines);
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
    CHECK(g_writes == 4 && g_elines == 2, "w=%d e=%d (first record; history write on boot 4, no transition)",
          g_writes, g_elines);
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
    CHECK(g_elines == 1 && g_writes == 3, "e=%d w=%d (first record + 2 brownouts)", g_elines, g_writes);
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
        fresh_nvs();
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
    fresh_nvs();
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
    CHECK(st() == PP_STATE_NORMAL && g_writes == 1, "battery_v 0 with a healthy rail never triggers");
    fresh_nvs();
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
    fresh_nvs();
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
    CHECK(st() == PP_STATE_NORMAL && g_writes == 1, "a 3.35 V idle rail with battery_v 0 must not enter");
    good_reads(3, 3250, 0);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_LOW_VOLTAGE, "a rail below 3.30 V enters");
#if HAVE_RECOVERY
    /* The normal probation is failed by the same entry rule: a 3.35 V idle
     * rail does not fail it. */
    fresh_nvs();
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
    fresh_nvs();
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

    fresh_nvs();
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
    fresh_nvs();
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
    fresh_nvs();
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
    fresh_nvs();
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
    CHECK(pp_encode(&p, v2, sizeof v2) == PP_BLOB_LEN && PP_BLOB_LEN == 128 && PP_BLOB_V1_LEN == 113, "lens");
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
    v2[PP_BLOB_LEN - 4 - 4 - 9] = 0;  /* has_low 0 with a low_utc: refused (CRC fixed below) */
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
    reboot(ESP_RST_POWERON, 5, true);   /* (after a power loss: no RTC mirror) */
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
    fresh_nvs();
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
        fresh_nvs();
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
    fresh_nvs();
    reboot(ESP_RST_BROWNOUT, 10, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    reboot(ESP_RST_INT_WDT, 11, true);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "a watchdog keeps the probation");
    reboot(ESP_RST_POWERON, 12, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_PROBE_FAILED && s_core.p.probe_fails == 1 &&
          s_core.p.hist_n == 1, "power-on on probation: failed (no brownout recorded)");
    CHECK(strcmp(g_last_e, "power_protect: normal -> hold reason=probe_failed") == 0, "%s", g_last_e);
    /* Garbage in RTC memory after a power-on never decodes. */
    fresh_nvs();
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
    fresh_nvs();
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
        fresh_nvs();
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
    fresh_nvs();
    reboot(ESP_RST_BROWNOUT, 3, false);
    const int w2 = g_writes;
    for (int i = 0; i < 2 * 720; i++) read5(POWER_READ_OK, 4100, 3500, true, true);
    CHECK(g_writes == w2 + 1, "undated: one write (%d)", g_writes - w2);
    g_src = "rtc";
    read5(POWER_READ_OK, 4100, 3500, true, true);
    CHECK(g_writes == w2 + 2, "dated once the clock is valid (%d)", g_writes - w2);
}

/* ── scenarios: one generation-ordered record (review of PR #113, round 2) ─ */

#if HAVE_RECOVERY
/* [P1] A failed probe that NVS refused stays protected through ANY number of
 * resets without power loss, for every such reset kind, with and without a
 * valid clock and a boot counter; and is put on record once NVS works. */
static void rtc_repeated_resets(void)
{
    static const esp_reset_reason_t kinds[] = { ESP_RST_SW, ESP_RST_TASK_WDT, ESP_RST_INT_WDT, ESP_RST_PANIC,
                                                ESP_RST_WDT };
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        for (int clk = 0; clk < 2; clk++) {
            for (int counter = 0; counter < 2; counter++) {
                fresh_nvs();
                g_fail_writes = false;
                reboot(ESP_RST_BROWNOUT, counter ? 2 : 0, clk);
                quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
                CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "setup");
                g_fail_writes = true;
                reboot(ESP_RST_BROWNOUT, counter ? 3 : 0, clk);
                CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails == 1, "failure in RAM");
                for (uint32_t r = 0; r < 3; r++) {
                    reboot(kinds[i], counter ? 4 + r : 0, clk);
                    CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails == 1 && s_core.persist_fault,
                          "kind %d clk %d counter %d reset %u: state %d fails %u", kinds[i], clk, counter, r, st(),
                          (unsigned)s_core.p.probe_fails);
                }
                /* NVS back: the next reset puts the record on NVS by itself. */
                g_fail_writes = false;
                reboot(kinds[i], counter ? 9 : 0, clk);
                CHECK(st() == PP_STATE_HOLD && !s_core.persist_fault, "synced at boot");
                reboot(ESP_RST_POWERON, counter ? 10 : 0, clk);
                CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails == 1, "and on record after a power loss");
            }
        }
    }
}

/* [P2] A reset at either NVS boundary, for a protective update and for a
 * relaxation: the newer record wins, an older RTC copy never overrides a
 * newer durable one, and a relaxation only exists if it reached NVS. */
static void reset_at_commit_boundary(void)
{
    /* The reviewer's case: an unwritten low record leaves the RTC copy ahead
     * (no_flash probation); NVS recovers; a probation failure (-> hold) is
     * committed and the unit resets before RAM/RTC are updated. */
    reboot(ESP_RST_BROWNOUT, 2, true);
    reboot(ESP_RST_BROWNOUT, 3, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NO_FLASH, "no_flash probation");
    g_fail_writes = true;
    good_reads(3, 4100, 3500);                          /* a low record NVS refuses */
    g_fail_writes = false;
    g_reset_at = 2;
    if (setjmp(g_reset_jump) == 0) read5(POWER_READ_OK, 3200, 3350, true, true);   /* probation fails */
    CHECK(g_reset_at == 0, "the reset was injected");
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails == 1 && s_core.p.reason == PP_REASON_PROBE_FAILED,
          "the durable hold wins over the older RTC copy (state %d fails %u)", st(), (unsigned)s_core.p.probe_fails);
    reboot(ESP_RST_TASK_WDT, 5, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails == 1, "and keeps winning");

    /* Protective update, reset BEFORE NVS stores it: the RTC copy is newer. */
    fresh_nvs();
    reboot(ESP_RST_BROWNOUT, 10, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL, "normal probation");
    g_reset_at = 1;
    if (setjmp(g_reset_jump) == 0) good_reads(3, 3250, 3900);   /* fails the probation -> no_flash */
    CHECK(g_reset_at == 0, "the reset was injected");
    reboot(ESP_RST_SW, 11, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1, "RTC copy ahead of NVS wins (state %d)", st());

    /* Relaxation, reset BEFORE NVS stores it: never happened. */
    fresh_nvs();
    reboot(ESP_RST_BROWNOUT, 20, true);
    g_reset_at = 1;
    if (setjmp(g_reset_jump) == 0) quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);   /* the probe step */
    CHECK(g_reset_at == 0, "the reset was injected");
    reboot(ESP_RST_SW, 21, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probation == PP_PROBATION_NONE, "an unwritten probe never happened");
    /* Relaxation, reset AFTER NVS committed it: it is on record, so it holds. */
    g_reset_at = 2;
    if (setjmp(g_reset_jump) == 0) quiet_reads(POWER_READ_NO_INPUT, 60);
    CHECK(g_reset_at == 0, "the reset was injected");
    reboot(ESP_RST_SW, 22, true);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "a committed probe holds");
}

/* [P2] A rejected relaxation keeps the protective facts of its read. */
static void rejected_relaxation_keeps_low(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    reboot(ESP_RST_BROWNOUT, 3, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probation == PP_PROBATION_NO_FLASH, "no_flash probation");
    advance_s(PP_PROBATION_S - (g_utc - s_core.p.since_utc) - 15);
    good_reads(2, 4100, 3900);
    advance_s(5);
    g_fail_writes = true;
    read5(POWER_READ_OK, 4100, 3500, true, true);   /* a low read AND the pass due; NVS refuses */
    CHECK(s_core.have_low && s_core.p.probation == PP_PROBATION_NO_FLASH, "pass refused, low kept");
    g_fail_writes = false;
    quiet_reads(POWER_READ_NO_INPUT, H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NONE && st() == PP_STATE_NO_FLASH, "passed after the hold");
    quiet_reads(POWER_READ_NO_INPUT, 4 * H);
    CHECK(st() == PP_STATE_NO_FLASH, "no probe within 6 h of the low read");
    quiet_reads(POWER_READ_NO_INPUT, H + 120);
    CHECK(st() == PP_STATE_NORMAL, "6 h after the low read: probe");
}

/* [P2] trusted -> untrusted low -> trusted reboot: the newest low still vetoes. */
static void low_clock_trust_change(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    good_reads(3, 4100, 3500);                      /* dated low */
    CHECK(s_core.p.has_low && s_core.p.low_utc != 0, "dated");
    reboot(ESP_RST_SW, 3, false);                   /* clock not trusted (hwm) */
    for (int i = 0; i < 8 * 720; i++) read5(POWER_READ_OK, 4100, 3500, true, true);   /* 8 h of lows */
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.has_low && s_core.p.low_utc == 0, "switched to undated");
    reboot(ESP_RST_SW, 4, true);                    /* trust back */
    quiet_reads(POWER_READ_NO_INPUT, 6 * H - 120);
    CHECK(st() == PP_STATE_NO_FLASH, "undated low: the quiet period counts from boot");
    quiet_reads(POWER_READ_NO_INPUT, 240);
    CHECK(st() == PP_STATE_NORMAL, "then the probe");
    /* The suspect-RTC path is the same: a low read while suspect undates it. */
    fresh_nvs();
    reboot(ESP_RST_BROWNOUT, 10, true);
    good_reads(3, 4100, 3500);
    g_suspect = true;
    good_reads(3, 4100, 3500);
    CHECK(s_core.p.has_low && s_core.p.low_utc == 0, "suspect RTC undates it");
    g_suspect = false;
    good_reads(3, 4100, 3500);
    CHECK(s_core.p.low_utc != 0, "and trust dates it again");
}

/* clear and a force DOWN are relaxations: on record first, or not at all.
 * A force UP applies at once and is kept in the RTC mirror. */
static void operator_relaxation_needs_record(void)
{
    reboot(ESP_RST_BROWNOUT, 2, true);
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD, "hold");
    g_fail_writes = true;
    CHECK(power_protect_clear() != ESP_OK && st() == PP_STATE_HOLD && s_core.p.hist_n == 2,
          "a refused clear changes nothing (state %d)", st());
    CHECK(power_protect_force(PP_STATE_NO_FLASH) != ESP_OK && st() == PP_STATE_HOLD,
          "a refused force down changes nothing");
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_HOLD, "and nothing comes back after a reset");
    /* force up with NVS refusing: applied, survives a reset, not a power loss */
    g_fail_writes = false;
    fresh_nvs();
    reboot(ESP_RST_POWERON, 5, true);              /* a first install: record + epoch */
    g_fail_writes = true;
    CHECK(power_protect_force(PP_STATE_HOLD) != ESP_OK && st() == PP_STATE_HOLD && s_core.p.forced, "forced up");
    reboot(ESP_RST_SW, 6, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "kept across a reset");
    reboot(ESP_RST_POWERON, 7, true);
    CHECK(st() == PP_STATE_NORMAL, "but not a power loss (never on record)");
    g_fail_writes = false;
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK && power_protect_force(PP_STATE_NO_FLASH) == ESP_OK &&
          st() == PP_STATE_NO_FLASH && power_protect_clear() == ESP_OK && st() == PP_STATE_NORMAL, "with NVS: all apply");
}
#endif

/* ── scenarios: round 3 (RTC rewrite, indeterminate NVS writes, NVS epoch) ─ */

#if HAVE_RECOVERY
/* A failed probe kept in RTC only (NVS refusing); the next reset adopts it
 * and its NVS sync fails too. */
static void rtc_only_failure(uint32_t boot)
{
    fresh_nvs();
    g_fail_writes = false;
    reboot(ESP_RST_BROWNOUT, boot, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "probation on record");
    g_fail_writes = true;
    good_reads(3, 3250, 3900);                           /* the probation fails: RTC only */
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1 && s_core.persist_fault, "failure in RTC only");
}

/* [P2] A reset INSIDE the RTC rewrite never loses the retained record. */
static void rtc_store_interrupted(void)
{
    /* The reviewer's case: the reset hits the boot's mirror rewrite after the
     * adopted record's NVS sync failed (the 2nd pp_encode of that boot). */
    rtc_only_failure(2);
    g_encode_reset_n = 2;
    if (setjmp(g_reset_jump) == 0) reboot(ESP_RST_SW, 3, true);
    g_encode_reset_n = 0;
    g_fail_writes = false;
    reboot(ESP_RST_TASK_WDT, 4, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1,
          "a reset in the boot rewrite must not restore the failed probation (state %d fails %u)", st(),
          (unsigned)s_core.p.probe_fails);
#if HAVE_GEN
    /* Every point inside rtc_store, on a run-time protective update (a force
     * up NVS refuses) on top of the RTC-only failure: the older slot carries
     * the failed probe through; the interrupted one is never armed. */
    for (int point = 1; point <= 3; point++) {
        rtc_only_failure(10);
        reboot(ESP_RST_SW, 11, true);                    /* adopted, sync refused */
        CHECK(st() == PP_STATE_NO_FLASH && s_core.persist_fault, "point %d: adopted", point);
        g_rtc_reset_at = point;
        if (setjmp(g_reset_jump) == 0) (void)power_protect_force(PP_STATE_HOLD);   /* -> rtc_store */
        CHECK(g_rtc_reset_at == 0, "point %d: injected", point);
        reboot(ESP_RST_INT_WDT, 12, true);
        CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1 && !s_core.p.forced,
              "point %d: still the failed probe (state %d fails %u)", point, st(), (unsigned)s_core.p.probe_fails);
        reboot(ESP_RST_PANIC, 13, true);
        CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1, "point %d: and after another reset", point);
    }
    /* And a rewrite that completes: the newer slot wins. */
    rtc_only_failure(20);
    reboot(ESP_RST_SW, 21, true);
    (void)power_protect_force(PP_STATE_HOLD);
    reboot(ESP_RST_INT_WDT, 22, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced && s_core.p.probe_fails == 1, "the completed rewrite wins");
#endif
    g_fail_writes = false;
}

/* [P2] An NVS write that stores the record and THEN reports an error
 * (ESP_ERR_NVS_REMOVE_FAILED): the re-read decides, and state and reply match
 * what NVS holds; when the re-read itself fails the relaxation is fenced. */
static void relaxation_write_then_error(void)
{
    /* operator clear */
    reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "durable forced hold");
    g_write_then_fail = 1;
    CHECK(power_protect_clear() == ESP_OK && st() == PP_STATE_NORMAL && !s_core.p.forced,
          "a clear that landed is reported as applied (state %d)", st());
    reboot(ESP_RST_SW, 2, true);
    CHECK(st() == PP_STATE_NORMAL && !s_core.p.forced, "and is what the next boot reads");
    /* operator force down */
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "hold again");
    g_write_then_fail = 1;
    CHECK(power_protect_force(PP_STATE_NO_FLASH) == ESP_OK && st() == PP_STATE_NO_FLASH, "a force down that landed");
    reboot(ESP_RST_SW, 3, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.forced, "on record");
    /* automatic: the probe step */
    fresh_nvs();
    reboot(ESP_RST_BROWNOUT, 4, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H - 60);
    g_write_then_fail = 1;
    quiet_reads(POWER_READ_NO_INPUT, 120);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL && !s_core.persist_fault,
          "a probe step that landed stands (state %d)", st());
    CHECK(strcmp(g_last_e, "power_protect: no_flash -> normal reason=probe") == 0, "%s", g_last_e);
    reboot(ESP_RST_SW, 5, true);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "and is on record");
    /* The re-read fails: cannot tell, so fence. The clear is refused and can
     * never win at a later boot, after a reset or a power loss. */
    fresh_nvs();
    reboot(ESP_RST_POWERON, 6, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "durable forced hold");
    g_write_then_fail = 1;
    g_get_err = ESP_FAIL;
    CHECK(power_protect_clear() != ESP_OK && st() == PP_STATE_HOLD && s_core.p.forced, "unknown outcome: refused");
    g_get_err = ESP_OK;
    reboot(ESP_RST_SW, 7, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "fenced across a reset (state %d)", st());
    reboot(ESP_RST_POWERON, 8, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "and across a power loss");
}

/* The RTC record belongs to one NVS lifetime: a wiped or rewritten NVS (new
 * epoch) never adopts it; an unreadable epoch does not count as a rewrite. */
static void rtc_bound_to_nvs_epoch(void)
{
    rtc_only_failure(2);
    reboot(ESP_RST_SW, 3, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1, "control: same NVS, adopted");
    /* flash_gui / clean_nvs rewrites NVS, then a USB / watchdog reset */
    g_fail_writes = false;
    fresh_nvs();
    g_epoch = 0;
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.hist_n == 0 && g_epoch != 0,
          "a rewritten NVS never brings the old record back (state %d)", st());
    reboot(ESP_RST_SW, 5, true);
    CHECK(st() == PP_STATE_NORMAL, "and stays so");
}
#endif

/* ── scenarios: round 4 (only a read of the NEW record proves it landed;
 *    no relaxation without a reset-safe RTC identity) ───────────────────── */

#if HAVE_RECOVERY
/* [P2] REMOVE_FAILED, then the immediate re-read shows NOTHING (mode 1) or
 * the OLD record (mode 2) while the new one is what the next boot reads:
 * neither proves "not landed", both are fenced. Operator clear and the
 * automatic probe step; after a reset and after a power loss. */
static void relaxation_reread_inconclusive(void)
{
    for (int mode = 1; mode <= 2; mode++) {
        fresh_nvs();
        reboot(ESP_RST_POWERON, 1, true);
        CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "mode %d: durable forced hold", mode);
        g_write_then_fail = 1;
        g_hide_after_write = mode;
        CHECK(power_protect_clear() != ESP_OK && st() == PP_STATE_HOLD && s_core.p.forced,
              "mode %d: an unproven clear is refused (state %d)", mode, st());
        g_hide_after_write = 0;
        reboot(ESP_RST_SW, 2, true);
        CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "mode %d: fenced across a reset (state %d)", mode, st());
        reboot(ESP_RST_POWERON, 3, true);
        CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "mode %d: and across a power loss (state %d)", mode, st());
        /* the automatic probe step */
        fresh_nvs();
        reboot(ESP_RST_BROWNOUT, 4, true);
        quiet_reads(POWER_READ_NO_INPUT, 6 * H - 60);
        g_write_then_fail = 1;
        g_hide_after_write = mode;
        quiet_reads(POWER_READ_NO_INPUT, 120);
        g_hide_after_write = 0;
        CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probation == PP_PROBATION_NONE,
              "mode %d: an unproven probe step never takes effect (state %d)", mode, st());
        CHECK(!s_core.persist_fault, "mode %d: the fence is on NVS: the durable record is the state", mode);
        reboot(ESP_RST_SW, 5, true);
        CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probation == PP_PROBATION_NONE, "mode %d: fenced (reset)", mode);
        reboot(ESP_RST_POWERON, 6, true);
        CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probation == PP_PROBATION_NONE, "mode %d: fenced (power loss)",
              mode);
    }
}

/* [P2] The first upgrade boot cannot create the epoch (transient NVS fault):
 * NVS recovers, a probe is taken, NVS refuses again and the probe fails. The
 * failure must survive a software reset. */
static void epoch_missing_first_boot(void)
{
    g_epoch = 0;
    g_fail_writes = true;
    reboot(ESP_RST_BROWNOUT, 2, true);
    CHECK(st() == PP_STATE_NO_FLASH, "protected on a brownout");
    g_fail_writes = false;
    good_reads(3, 3500, 3500);                         /* a low record: NVS takes it */
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "probation on record");
    CHECK(g_epoch != 0, "the epoch was created once NVS took writes again");
    g_fail_writes = true;
    good_reads(3, 3250, 3900);                         /* the probation fails; NVS refuses */
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1, "failure in RAM");
    reboot(ESP_RST_SW, 3, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1,
          "the failure survives a software reset (state %d fails %u)", st(), (unsigned)s_core.p.probe_fails);
    g_fail_writes = false;
    /* An epoch write that lands but reports an error is read back. */
    fresh_nvs();
    g_epoch = 0;
    g_epoch_lands_error = true;
    reboot(ESP_RST_BROWNOUT, 10, true);
    g_epoch_lands_error = false;
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "probation on record");
    g_fail_writes = true;
    good_reads(3, 3250, 3900);
    reboot(ESP_RST_SW, 11, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probe_fails == 1, "landed-with-error epoch is used");
    g_fail_writes = false;
}

/* No epoch at all (its write keeps failing while state writes work): no
 * automatic relaxation, until it can be created. */
static void no_epoch_no_relaxation(void)
{
    g_epoch = 0;
    g_fail_epoch_writes = true;
    reboot(ESP_RST_BROWNOUT, 2, true);
    quiet_reads(POWER_READ_NO_INPUT, 12 * H);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.probation == PP_PROBATION_NONE,
          "no probe without a reset-safe identity (state %d)", st());
    good_reads(READS_30MIN + 30, 4100, 3900);
    CHECK(st() == PP_STATE_NO_FLASH, "no accumulated exit either");
    g_fail_epoch_writes = false;
    good_reads(800, 4100, 3900);                   /* past the 1-h hold, still qualifying */
    CHECK(g_epoch != 0 && st() == PP_STATE_NORMAL && s_core.p.reason == PP_REASON_POWER_RECOVERED,
          "once the epoch exists, the exit commits (state %d)", st());
    /* and the probe likewise */
    fresh_nvs();
    g_epoch = 0;
    g_fail_epoch_writes = true;
    reboot(ESP_RST_BROWNOUT, 3, true);
    quiet_reads(POWER_READ_NO_INPUT, 7 * H);
    CHECK(st() == PP_STATE_NO_FLASH, "held");
    g_fail_epoch_writes = false;
    quiet_reads(POWER_READ_NO_INPUT, H + 60);
    CHECK(g_epoch != 0 && st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL,
          "then the probe (state %d)", st());
}
#endif

#ifdef PP_NATIVE_NVS
#ifndef POWER_PROTECT_ERR_UNCONFIRMED   /* (so the matrix also runs against e0d4e2e) */
#define POWER_PROTECT_ERR_UNCONFIRMED ESP_ERR_NOT_FINISHED
#endif
/* A relaxation (operator clear of a forced hold; the automatic probe step)
 * against the REAL IDF NVS, with a fault at every raw entry-table write and
 * every read of the attempt in turn. Whatever the IDF reports, the outcome
 * must be one of: it stands (ok, and every later boot reads it), or it is
 * refused and fenced on NVS (persist_failed, and every later boot, after a
 * reset AND after a power loss, is still protected), or the fence is not
 * confirmed (persist_unconfirmed: protected now and after a reset). Prints
 * one NATIVE line per fault point for test_power_protect.py. */
static void native_relaxation_matrix(void)
{
    int n_ok = 0, n_refused = 0, n_unconfirmed = 0;
    for (int kind = 0; kind < 2; kind++) {
        for (int at = 1; at <= 16; at++) {
            /* operator clear */
            native_format();
            reboot(ESP_RST_POWERON, 1, true);
            CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "setup");
            native_fault(kind == 0 ? at : 0, kind == 1 ? at : 0);
            const esp_err_t err = power_protect_clear();
            native_fault(0, 0);
            const pp_state_t now = st();
            reboot(ESP_RST_SW, 2, true);
            const pp_state_t after_reset = st();
            const bool forced_reset = s_core.p.forced;
            reboot(ESP_RST_POWERON, 3, true);
            const pp_state_t after_loss = st();
            /* the same fault, then a power loss straight away (no reset in
             * between to sync the RTC fence to NVS) */
            native_format();
            reboot(ESP_RST_POWERON, 4, true);
            CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "setup");
            native_fault(kind == 0 ? at : 0, kind == 1 ? at : 0);
            const esp_err_t err2 = power_protect_clear();
            native_fault(0, 0);
            reboot(ESP_RST_POWERON, 5, true);
            const pp_state_t direct_loss = st();
            CHECK(err2 == err, "deterministic");
            printf("NATIVE clear %s_fault=%d result=%#x now=%s reset=%s power_loss=%s direct_loss=%s\n",
                   kind ? "read" : "write", at, (unsigned)err, pp_state_name(now), pp_state_name(after_reset),
                   pp_state_name(after_loss), pp_state_name(direct_loss));
            if (err == ESP_OK) {
                n_ok++;
                CHECK(now == PP_STATE_NORMAL && after_reset == PP_STATE_NORMAL && after_loss == PP_STATE_NORMAL &&
                      direct_loss == PP_STATE_NORMAL,
                      "%s %d: an applied clear stands", kind ? "read" : "write", at);
            } else if (err == POWER_PROTECT_ERR_UNCONFIRMED) {
                n_unconfirmed++;
                CHECK(now == PP_STATE_HOLD && after_reset == PP_STATE_HOLD && forced_reset,
                      "%s %d: unconfirmed, still protected now and after a reset", kind ? "read" : "write", at);
            } else {
                n_refused++;
                CHECK(now == PP_STATE_HOLD && after_reset == PP_STATE_HOLD && forced_reset &&
                      after_loss == PP_STATE_HOLD && direct_loss == PP_STATE_HOLD,
                      "%s %d: a refused clear never wins (now %d reset %d loss %d)", kind ? "read" : "write", at, now,
                      after_reset, after_loss);
            }
            /* the automatic probe step */
            native_format();
            reboot(ESP_RST_BROWNOUT, 10, true);
            quiet_reads(POWER_READ_NO_INPUT, 6 * H - 60);
            native_fault(kind == 0 ? at : 0, kind == 1 ? at : 0);
            quiet_reads(POWER_READ_NO_INPUT, 120);
            native_fault(0, 0);
            const bool probed = st() == PP_STATE_NORMAL;
            reboot(ESP_RST_SW, 11, true);
            const pp_state_t p_reset = st();
            reboot(ESP_RST_POWERON, 12, true);
            const pp_state_t p_loss = st();
            printf("NATIVE probe %s_fault=%d probed=%d reset=%s power_loss=%s\n", kind ? "read" : "write", at,
                   probed, pp_state_name(p_reset), pp_state_name(p_loss));
            if (probed) {
                CHECK(p_reset == PP_STATE_NORMAL, "%s %d: a taken probe is on record", kind ? "read" : "write", at);
            } else if (!s_core.persist_fault) {
                CHECK(p_reset == PP_STATE_NO_FLASH, "%s %d: an untaken probe never appears (reset)",
                      kind ? "read" : "write", at);
            }
        }
    }
    printf("NATIVE_SUMMARY ok=%d refused=%d unconfirmed=%d\n", n_ok, n_refused, n_unconfirmed);
    CHECK(n_refused > 0, "the matrix must hit the REMOVE_FAILED path");
}
#endif

#if HAVE_RECOVERY
/* The fence itself is refused (documented limit, round 4): the reply says the
 * outcome is UNCONFIRMED; the unit is protected now and after a reset (RTC
 * fence), and after a POWER LOSS the clear that did land wins: total NVS
 * failure cannot be made durable. */
static void fence_unconfirmed(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "durable forced hold");
    g_write_then_fail = 1;
    g_hide_after_write = 1;
    g_refuse_after_landed = true;
    const esp_err_t err = power_protect_clear();
#if HAVE_EPOCH_R4
    CHECK(err == POWER_PROTECT_ERR_UNCONFIRMED, "the reply says unconfirmed (%#x)", (unsigned)err);
#endif
    CHECK(err != ESP_OK && st() == PP_STATE_HOLD && s_core.p.forced && s_core.persist_fault, "still protected");
    g_hide_after_write = 0;
    /* NVS still refusing: a reset keeps the RTC fence... */
    g_fail_writes = true;
    reboot(ESP_RST_SW, 2, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "after a reset: the RTC fence holds (state %d)", st());
    /* ...but a power loss before NVS takes any write reads the landed clear. */
    reboot(ESP_RST_POWERON, 3, true);
    CHECK(st() == PP_STATE_NORMAL, "after a power loss the landed clear wins: the documented limit");
    g_fail_writes = false;
    g_refuse_after_landed = false;
    /* Once NVS takes writes again, a reset in between puts the fence on NVS
     * (the boot sync of the adopted RTC record) and the limit is closed. */
    fresh_nvs();
    reboot(ESP_RST_POWERON, 4, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "durable forced hold");
    g_write_then_fail = 1;
    g_hide_after_write = 1;
    g_refuse_after_landed = true;
    CHECK(power_protect_clear() != ESP_OK, "unconfirmed");
    g_hide_after_write = 0;
    g_refuse_after_landed = false;
    reboot(ESP_RST_SW, 5, true);
    reboot(ESP_RST_POWERON, 6, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "synced fence survives a later power loss (state %d)", st());
}

static void cmd_init(void);
static void cmd(const char *json, bool run);
static void cmd_persist_unconfirmed(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    cmd_init();
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "durable forced hold");
    g_write_then_fail = 1;
    g_hide_after_write = 1;
    g_refuse_after_landed = true;
    cmd("{\"type\":\"power_protect\",\"id\":\"u1\",\"action\":\"clear\"}", true);
    g_hide_after_write = 0;
    g_refuse_after_landed = false;
    CHECK(st() == PP_STATE_HOLD, "still protected");
}
#endif

#ifdef PP_NATIVE_NVS
/* Round 5: TWO transient raw-write faults across the relaxation and its fence
 * writes (the reviewer's #2/#4 case: candidate fails, first fence fails,
 * second fence clean -> was "confirmed", and after a re-init NVS held NO
 * record -> normal). Every pair a in 1..16, b in a+1..24, then a direct power
 * loss: a clear reported refused must never come back at full load, and
 * persist_failed must mean the forced hold is what NVS holds. */
static void native_two_fault_scan(void)
{
    int n_ok = 0, n_failed = 0, n_unconfirmed = 0, n_full_load = 0;
    for (int a = 1; a <= 16; a++) {
        for (int b = a + 1; b <= 24; b++) {
            native_format();
            reboot(ESP_RST_POWERON, 1, true);
            CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "setup");
            native_fault2(a, b);
            const esp_err_t err = power_protect_clear();
            native_fault(0, 0);
            reboot(ESP_RST_POWERON, 2, true);
            const pp_state_t after = st();
            const bool forced = s_core.p.forced;
            if (a == 2 && b == 4) {
                printf("NATIVE2 reviewer_case a=2 b=4 result=%#x after_power_loss=%s forced=%d reason=%s\n",
                       (unsigned)err, pp_state_name(after), forced, pp_reason_name((pp_reason_t)s_core.p.reason));
            }
            if (err == ESP_OK) {
                n_ok++;
                CHECK(after == PP_STATE_NORMAL, "a=%d b=%d: an applied clear stands", a, b);
            } else if (err == POWER_PROTECT_ERR_UNCONFIRMED) {
                n_unconfirmed++;
                if (after == PP_STATE_NORMAL) n_full_load++;   /* the documented limit */
            } else {
                n_failed++;
                CHECK(after == PP_STATE_HOLD && forced,
                      "a=%d b=%d: persist_failed must mean the fence is on NVS (after %d forced %d reason %d)", a, b,
                      after, forced, s_core.p.reason);
            }
        }
    }
    printf("NATIVE2_SUMMARY pairs=%d ok=%d failed=%d unconfirmed=%d unconfirmed_full_load=%d\n",
           n_ok + n_failed + n_unconfirmed, n_ok, n_failed, n_unconfirmed, n_full_load);
}
#endif

/* ── scenarios: round 5 (missing record, unverified epoch) ──────────────── */

#if HAVE_RECOVERY
/* [P1b] An epoch says this module wrote a record into this NVS: a missing
 * record then means the state was LOST, never "first install". */
static void missing_record_fails_closed(void)
{
    fresh_nvs();
    reboot(ESP_RST_POWERON, 1, true);
    CHECK(st() == PP_STATE_NORMAL && g_epoch != 0 && g_blob_len == PP_BLOB_LEN, "first install: record + epoch");
    g_blob_len = 0;                                      /* the record is gone, the epoch is not */
    reboot(ESP_RST_POWERON, 2, true);
#if HAVE_R5
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_STATE_MISSING && s_core.persist_fault,
          "epoch + no record: no_flash, state_missing (state %d)", st());
#else
    CHECK(st() == PP_STATE_NO_FLASH, "epoch + no record: no_flash (state %d)", st());
#endif
    /* an unreadable epoch beside a missing record: also not a first install */
    g_blob_len = 0;
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_POWERON, 3, true);
    g_epoch_get_err = ESP_OK;
    CHECK(st() == PP_STATE_NO_FLASH, "unreadable epoch + no record: no_flash (state %d)", st());
    /* no epoch and no record: a first install (or a wiped NVS), normal */
    fresh_nvs();
    reboot(ESP_RST_POWERON, 4, true);
    CHECK(st() == PP_STATE_NORMAL && !s_core.persist_fault, "no epoch + no record: normal");
    /* a first install whose record write fails creates no epoch: next boot is
     * a first install again, not a lost state */
    fresh_nvs();
    g_fail_writes = true;
    reboot(ESP_RST_POWERON, 5, true);
    CHECK(g_epoch == 0, "no epoch without a record");
    g_fail_writes = false;
    reboot(ESP_RST_POWERON, 6, true);
    CHECK(st() == PP_STATE_NORMAL && g_epoch != 0, "then a clean first install");
    /* an upgrade from the first release: a v1 record, no epoch: epoch created */
}

/* Put a record into NVS as another lifetime wrote it. */
static void nvs_put(const pp_persist_t *p)
{
    CHECK(pp_encode(p, g_blob, sizeof g_blob) == PP_BLOB_LEN, "encode");
    g_blob_len = PP_BLOB_LEN;
}

/* U1 (round 7): an unreadable epoch fails closed and stays passive. */
static void u1_unverified_is_passive(void)
{
    /* a durable forced hold, RTC slots of this lifetime */
    fresh_nvs();
    reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "forced hold");
    pp_rtc_slot_t rtc_before[2];
    memcpy(rtc_before, s_rtc, sizeof rtc_before);
    const int w = g_writes;
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_SW, 2, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced && s_core.persist_fault, "NVS forced hold kept");
    quiet_reads(POWER_READ_NO_INPUT, 30 * H);
    CHECK(st() == PP_STATE_HOLD, "no relaxation of any kind");
#ifdef POWER_PROTECT_ERR_UNVERIFIED
    CHECK(power_protect_clear() == POWER_PROTECT_ERR_UNVERIFIED && st() == PP_STATE_HOLD, "clear refused");
    CHECK(power_protect_force(PP_STATE_NO_FLASH) == POWER_PROTECT_ERR_UNVERIFIED && st() == PP_STATE_HOLD,
          "force down refused");
#endif
    reboot(ESP_RST_SW, 4, true);                         /* (a reset keeps RTC memory) */
    CHECK(memcmp(rtc_before, s_rtc, sizeof rtc_before) == 0, "RTC slots never touched");
    reboot(ESP_RST_BROWNOUT, 3, true);                   /* a brownout while unverified: RAM only */
    CHECK(st() == PP_STATE_HOLD, "still hold");
    CHECK(g_writes == w, "nothing written to NVS (%d)", g_writes - w);
    /* NVS normal -> no_flash, identity_unverified; force up applies in RAM */
    g_epoch_get_err = ESP_OK;
    fresh_nvs();
    reboot(ESP_RST_POWERON, 5, true);
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_SW, 6, true);
#ifdef POWER_PROTECT_ERR_UNVERIFIED
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_IDENTITY_UNVERIFIED, "floor: no_flash");
#else
    CHECK(st() == PP_STATE_NO_FLASH, "floor: no_flash (state %d)", st());
#endif
#ifdef POWER_PROTECT_ERR_UNVERIFIED
    CHECK(power_protect_force(PP_STATE_HOLD) == POWER_PROTECT_ERR_UNVERIFIED && st() == PP_STATE_HOLD &&
          s_core.p.forced, "force up applies in RAM");
#endif
    /* NVS unreadable too: no_flash, state_unreadable, whatever RTC holds */
    g_blob[9] ^= 0x55;
    reboot(ESP_RST_SW, 7, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_STATE_UNREADABLE, "unreadable: no_flash");
    g_epoch_get_err = ESP_OK;
}

/* U2: verification reconciles (a) a probation as failed, (b) restarts the
 * quiet period, (c) never goes below the U1 state; then it is persisted. */
static void u2_verification_reconciles(void)
{
    /* (a) + (b) */
    fresh_nvs();
    reboot(ESP_RST_BROWNOUT, 1, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(s_core.p.probation == PP_PROBATION_NORMAL, "probation on record");
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_SW, 2, true);
    CHECK(st() == PP_STATE_NO_FLASH, "U1 floor");
    g_epoch_get_err = ESP_OK;
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);         /* due: epoch_ensure verifies (U2) */
    CHECK(s_epoch_verified && st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_PROBE_FAILED &&
          s_core.p.probe_fails == 1 && s_core.p.has_low, "(a) a hidden probation counts as failed (state %d)", st());
    pp_persist_t q;
    CHECK(pp_decode(&q, g_blob, g_blob_len) && q.state == PP_STATE_HOLD && q.probe_fails == 1 && q.has_low,
          "persisted");
    quiet_reads(POWER_READ_NO_INPUT, 11 * H);
    CHECK(st() == PP_STATE_HOLD, "(b) the quiet period restarted at verification (level 1: 12 h)");
    quiet_reads(POWER_READ_NO_INPUT, H + 120);
    CHECK(st() == PP_STATE_NO_FLASH, "then the probe (state %d)", st());
    /* (c) */
    fresh_nvs();
    reboot(ESP_RST_POWERON, 10, true);
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_SW, 11, true);
    (void)power_protect_force(PP_STATE_HOLD);           /* RAM only */
    g_epoch_get_err = ESP_OK;
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK && st() == PP_STATE_HOLD && s_core.p.forced,
          "(c) verified: the U1 forced hold is kept and written");
    CHECK(pp_decode(&q, g_blob, g_blob_len) && q.state == PP_STATE_HOLD && q.forced, "on record");
}

/* Round 6 cases, now under U1/U2. */
static void same_epoch_low_veto(void)
{
    fresh_nvs();
    reboot(ESP_RST_BROWNOUT, 2, true);                   /* no_flash */
    quiet_reads(POWER_READ_NO_INPUT, 6 * H - 60);
    g_fail_writes = true;
    good_reads(3, 4100, 3500);                           /* a fresh low: NVS refuses, RTC keeps it */
    g_fail_writes = false;
    g_epoch_get_err = ESP_FAIL;                          /* only the epoch read fails, transiently */
    reboot(ESP_RST_SW, 3, true);
    g_epoch_get_err = ESP_OK;
    quiet_reads(POWER_READ_NO_INPUT, 120);
    CHECK(st() == PP_STATE_NO_FLASH, "the low veto survives an unverified boot (state %d)", st());
    quiet_reads(POWER_READ_NO_INPUT, 6 * H - 300);
    CHECK(st() == PP_STATE_NO_FLASH, "the quiet period restarted at verification");
    quiet_reads(POWER_READ_NO_INPUT, 600);
    CHECK(st() == PP_STATE_NORMAL, "then the probe (state %d)", st());
}

/* An RTC slot as another lifetime left it (its own epoch). */
static void rtc_put(int slot, const pp_persist_t *p, uint32_t epoch)
{
    s_rtc[slot].magic = 0;
    CHECK(pp_encode(p, s_rtc[slot].blob, sizeof s_rtc[slot].blob) == PP_BLOB_LEN, "encode");
    s_rtc[slot].epoch = epoch;
    s_rtc[slot].magic = PP_RTC_MAGIC;
}

/* A foreign gen-17 record never decides anything; while unverified the unit
 * is >= no_flash, after verification the current lifetime's NVS decides. */
static void slot_shadow(void)
{
    fresh_nvs();
    memset(&s_rtc, 0, sizeof s_rtc);
    pp_persist_t old;
    memset(&old, 0, sizeof old);
    old.state = PP_STATE_NORMAL; old.reason = PP_REASON_CLEARED; old.gen = 17;
    rtc_put(0, &old, 0x01D0001u);
    pp_persist_t cur;
    memset(&cur, 0, sizeof cur);
    cur.gen = 1; cur.state = PP_STATE_HOLD; cur.forced = true; cur.reason = PP_REASON_FORCED;
    nvs_put(&cur);
    g_epoch = 0x0E70002u;
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_SW, 2, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "unverified: NVS's forced hold, the foreign record ignored");
    g_epoch_get_err = ESP_OK;
    reboot(ESP_RST_SW, 3, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "verified: the foreign gen 17 never shadows it (state %d)", st());
}

/* ── the reviewer's round-7 repros, now required to be safe ────────────── */

static void r7_probation_merge(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    pp_persist_t current; memset(&current, 0, sizeof current);
    current.state = PP_STATE_NORMAL; current.reason = PP_REASON_PROBE;
    current.probation = PP_PROBATION_NORMAL; current.since_utc = (int64_t)g_utc - 60; current.gen = 20;
    nvs_put(&current);
    pp_persist_t stale; memset(&stale, 0, sizeof stale);
    stale.state = PP_STATE_NORMAL; stale.reason = PP_REASON_CLEARED; stale.gen = 1;
    memset(&s_rtc, 0, sizeof s_rtc); rtc_put(0, &stale, g_epoch);
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
    CHECK(st() == PP_STATE_NO_FLASH, "U1 floor");
    g_epoch_get_err = ESP_OK;
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails == 1, "a brownout on probation fails the probe");
}

static void r7_history_merge(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    pp_persist_t current; memset(&current, 0, sizeof current);
    current.gen = 20; current.state = PP_STATE_NO_FLASH; current.reason = PP_REASON_BROWNOUT;
    current.since_utc = (int64_t)g_utc - 24 * H; current.hist_n = 1;
    current.hist[0] = (pp_brownout_t){ (int64_t)g_utc - 60, 10 };
    nvs_put(&current);
    pp_persist_t stale = current; stale.gen = 1; stale.hist[0] = (pp_brownout_t){ (int64_t)g_utc - 24 * H, 1 };
    memset(&s_rtc, 0, sizeof s_rtc); rtc_put(0, &stale, g_epoch ^ 0x1111);
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 11, true);
    g_epoch_get_err = ESP_OK;
    quiet_reads(POWER_READ_NO_INPUT, 2 * H);
    CHECK(st() == PP_STATE_NO_FLASH, "a recent brownout vetoes the probe for 6 h");
}

static void r7_clear_reverify(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "setup");
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
    CHECK(!s_epoch_verified && st() == PP_STATE_HOLD, "unverified");
    g_epoch_get_err = ESP_OK;
    const esp_err_t err = power_protect_clear();
    pp_persist_t durable; CHECK(pp_decode(&durable, g_blob, g_blob_len), "decode");
    CHECK(err == ESP_OK && st() == PP_STATE_NORMAL && s_core.p.state == PP_STATE_NORMAL &&
          durable.state == PP_STATE_NORMAL, "a clear once the epoch reads stands (err %#x)", (unsigned)err);
}

static void r7_verify_rtc_interrupt(void)
{
    fresh_nvs(); reboot(ESP_RST_BROWNOUT, 1, true);
    quiet_reads(POWER_READ_NO_INPUT, 3 * H);
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
    g_fail_writes = true;
    good_reads(3, 4100, 3500);                           /* RAM only while unverified */
    g_epoch_get_err = ESP_OK;
    g_rtc_reset_at = 2;
    if (setjmp(g_reset_jump) == 0) (void)epoch_ensure();
    g_rtc_reset_at = 0;
    g_fail_writes = false;
    reboot(ESP_RST_SW, 3, true);
    /* stated limit: U1 facts are RAM-only until verification is on record;
     * the unit stays protective */
    CHECK(st() >= PP_STATE_NO_FLASH, "fail-closed after a reset inside verification (state %d)", st());
}

static void r7_probation_natural(void)
{
    fresh_nvs(); reboot(ESP_RST_BROWNOUT, 1, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NORMAL && s_core.p.probation == PP_PROBATION_NORMAL, "probe setup");
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
    CHECK(st() == PP_STATE_NO_FLASH, "U1 floor");
    g_epoch_get_err = ESP_OK; reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.probe_fails == 1, "a brownout during the probation fails the probe");
}

static void r7_history_natural(void)
{
    fresh_nvs();
    for (unsigned boot = 1; boot <= 9; boot++) {
        reboot(ESP_RST_BROWNOUT, boot, true);
        if (boot < 9) advance_s(24 * H);
    }
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 10, true);
    g_epoch_get_err = ESP_OK; quiet_reads(POWER_READ_NO_INPUT, 60);
    CHECK(st() == PP_STATE_NO_FLASH, "a brownout a minute ago vetoes the probe");
}

static void r7_clear_unreadable(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "setup");
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
#ifdef POWER_PROTECT_ERR_UNVERIFIED
    CHECK(power_protect_clear() == POWER_PROTECT_ERR_UNVERIFIED && st() == PP_STATE_HOLD,
          "a clear needs a readable epoch");
#endif
    g_epoch_get_err = ESP_OK; reboot(ESP_RST_SW, 3, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "still the forced hold");
    CHECK(power_protect_clear() == ESP_OK && st() == PP_STATE_NORMAL, "then the clear applies");
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_NORMAL && !s_core.p.forced, "and stands");
}

static void r7_verify_rtc_interrupt_hold(void)
{
    fresh_nvs(); reboot(ESP_RST_BROWNOUT, 1, true);
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
    g_fail_writes = true; good_reads(3, 4100, 3500);
    (void)power_protect_force(PP_STATE_HOLD);            /* RAM only */
    CHECK(st() == PP_STATE_HOLD, "forced hold in RAM");
    g_epoch_get_err = ESP_OK; g_rtc_reset_at = 2;
    if (setjmp(g_reset_jump) == 0) reboot(ESP_RST_SW, 3, true);
    g_rtc_reset_at = 0;
    g_fail_writes = false; reboot(ESP_RST_SW, 4, true);
    /* stated limit: the RAM-only forced hold of the U1 period is lost by a
     * reset; the unit is still protective */
    CHECK(st() >= PP_STATE_NO_FLASH, "fail-closed (state %d)", st());
}

static void r7_clear_reply(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "setup");
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
    g_epoch_get_err = ESP_OK; cmd_init();
    cmd("{\"type\":\"power_protect\",\"id\":\"review\",\"action\":\"clear\"}", true);
    CHECK(st() == PP_STATE_NORMAL, "the reply corresponds to the cleared state");
}

static void r7_merge_gen(void)
{
    fresh_nvs(); reboot(ESP_RST_BROWNOUT, 1, true);
    pp_persist_t newer = s_core.p; newer.gen = 17;
    memset(&s_rtc, 0, sizeof s_rtc); rtc_put(0, &newer, g_epoch);
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
    g_epoch_get_err = ESP_OK; quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(st() == PP_STATE_NO_FLASH, "verification restarted the quiet period");
    quiet_reads(POWER_READ_NO_INPUT, 6 * H);
    CHECK(st() == PP_STATE_NORMAL, "probe");
    reboot(ESP_RST_SW, 3, true);
    CHECK(st() == PP_STATE_NORMAL, "a verified relaxation is never shadowed by an older record");
}

static void r7_since_merge(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    pp_persist_t a; memset(&a, 0, sizeof a);
    a.state = PP_STATE_NO_FLASH; a.reason = PP_REASON_LOW_VOLTAGE; a.since_utc = (int64_t)g_utc - 24 * H; a.gen = 1;
    nvs_put(&a);
    pp_persist_t b = a; b.since_utc = (int64_t)g_utc - 60; b.gen = 17;
    memset(&s_rtc, 0, sizeof s_rtc); rtc_put(0, &b, g_epoch ^ 0x1111);
    g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
    g_epoch_get_err = ESP_OK; quiet_reads(POWER_READ_NO_INPUT, 60);
    CHECK(st() == PP_STATE_NO_FLASH, "no probe a minute after an unverified period");
}

static void r7_merge_gen_natural(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "hold setup");
    g_fail_writes = true;
    CHECK(power_protect_clear() == POWER_PROTECT_ERR_UNCONFIRMED, "clear and fences refused");
    g_fail_writes = false; g_epoch_get_err = ESP_FAIL; reboot(ESP_RST_SW, 2, true);
    g_epoch_get_err = ESP_OK;
    CHECK(power_protect_clear() == ESP_OK && st() == PP_STATE_NORMAL, "clear accepted once the epoch reads");
    reboot(ESP_RST_SW, 3, true);
    CHECK(st() == PP_STATE_NORMAL, "the old fence never outranks a later accepted clear");
}
#endif

/* ── round 8: this boot's own U1 observations carry into U2 ───────────── */

#if HAVE_RECOVERY
/* A brownout recorded during U1 links with the next one after an automatic
 * verification (no operator, no write failure). */
static void r8_u1_brownout_auto_verification(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_BROWNOUT, 2, true);
    CHECK(s_unverified && s_core.p.hist_n == 1, "the brownout lives in U1 RAM");
    g_epoch_get_err = ESP_OK;
    good_reads(READS_30MIN + 4, 4100, 3900);
    CHECK(s_epoch_verified && st() == PP_STATE_NO_FLASH && s_core.p.hist_n == 1, "U2 carries it (hist %u)",
          (unsigned)s_core.p.hist_n);
    pp_persist_t q;
    CHECK(pp_decode(&q, g_blob, g_blob_len) && q.hist_n == 1 && q.hist[0].boot == 2, "and it is on record");
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD, "two brownouts 30 min apart link after the verification (state %d)", st());
}

/* The same through an operator force, with a forced no_flash. */
static void r8_u1_brownout_survives_verification(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_BROWNOUT, 2, true);
    g_epoch_get_err = ESP_OK;
    CHECK(power_protect_force(PP_STATE_NO_FLASH) == ESP_OK, "verify and force no_flash");
    CHECK(s_core.p.hist_n == 1, "history carried (%u)", (unsigned)s_core.p.hist_n);
    advance_s(60);
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD, "two brownouts a minute apart escalate, even forced (state %d)", st());
}

/* A force set during U1 survives U2 (forced OR), also when the retried force
 * is refused, and a forced state never auto-exits afterwards. */
static void r8_u1_force_survives_verification(void)
{
    for (int variant = 0; variant < 2; variant++) {
        fresh_nvs(); reboot(ESP_RST_BROWNOUT, 1, true);
        g_fail_writes = true;
        advance_s(60); reboot(ESP_RST_BROWNOUT, 2, true);   /* hold in RTC, NVS no_flash */
        CHECK(st() == PP_STATE_HOLD && !s_core.p.forced, "v%d: RTC hold", variant);
        g_fail_writes = false; g_epoch_get_err = ESP_FAIL;
        reboot(ESP_RST_SW, 3, true);
        CHECK(st() == PP_STATE_NO_FLASH && !s_core.p.forced, "v%d: U1 reads NVS no_flash", variant);
        (void)power_protect_force(PP_STATE_NO_FLASH);
        CHECK(s_core.p.forced, "v%d: forced in RAM", variant);
        g_epoch_get_err = ESP_OK;
        g_fail_writes = true;
        (void)power_protect_force(PP_STATE_NO_FLASH);         /* verifies (U2), then the force: refused */
        CHECK(st() == PP_STATE_HOLD && s_core.p.forced,
              "v%d: U2 = the RTC hold, forced kept (state %d forced %d)", variant, st(), s_core.p.forced);
        g_fail_writes = false;
        if (variant == 1) {
            reboot(ESP_RST_SW, 4, true);                      /* the RTC mirror agrees with RAM */
            CHECK(st() == PP_STATE_HOLD && s_core.p.forced, "v1: RAM and the durable record agree on forced");
        }
        good_reads(READS_30MIN + 4, 4100, 3900);
        CHECK(st() != PP_STATE_NORMAL && s_core.p.forced, "v%d: a forced state never auto-exits (state %d)",
              variant, st());
    }
}

/* U1 -> U2 -> U1 -> U2 across boots: each boot's brownout is carried; and
 * once verified, a boot never returns to U1 (the identity latches). */
static void r8_u1_u2_cycles(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_BROWNOUT, 2, true);
    g_epoch_get_err = ESP_OK;
    good_reads(READS_30MIN + 4, 4100, 3900);              /* U2 #1 */
    CHECK(s_epoch_verified && s_core.p.hist_n == 1, "first carried");
    g_epoch_get_err = ESP_FAIL;                           /* the epoch read fails again this boot */
    quiet_reads(POWER_READ_NO_INPUT, H + 60);             /* past the low record's refresh hour */
    const int w = g_writes;
    good_reads(3, 4100, 3500);                            /* a low record: still written */
    CHECK(s_epoch_verified && !s_unverified && g_writes > w, "verified latches for the boot");
    advance_s(20 * 60);
    reboot(ESP_RST_BROWNOUT, 3, true);                    /* U1 again, linked in RAM */
    CHECK(s_unverified && st() == PP_STATE_HOLD, "U1: linked to the carried brownout (state %d)", st());
    g_epoch_get_err = ESP_OK;
    good_reads(READS_30MIN + 4, 4100, 3900);              /* U2 #2 */
    CHECK(s_epoch_verified && st() == PP_STATE_HOLD && s_core.p.hist_n == 2, "both carried, hold (hist %u)",
          (unsigned)s_core.p.hist_n);
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.hist_n == 2, "on record");
}

/* A full history: the U1 brownout goes in newest-last, the oldest drops. */
static void r8_u1_brownout_history_cap(void)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    for (uint32_t b = 2; b < 2 + PP_HIST_MAX; b++) {
        reboot(ESP_RST_BROWNOUT, b, true);
        advance_s(7 * H);
    }
    CHECK(s_core.p.hist_n == PP_HIST_MAX, "full");
    const uint32_t oldest_next = s_core.p.hist[1].boot;
    g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_BROWNOUT, 50, true);
    g_epoch_get_err = ESP_OK;
    (void)power_protect_force(PP_STATE_HOLD);              /* verifies */
    CHECK(s_core.p.hist_n == PP_HIST_MAX && s_core.p.hist[PP_HIST_MAX - 1].boot == 50 &&
          s_core.p.hist[0].boot == oldest_next, "newest last, the oldest dropped");
}
/* Round 9: an UNKNOWN-identity brownout (no clock, boot counter 0) recorded
 * during U1 is carried, never de-duplicated against an older unknown entry;
 * the next brownout then escalates. Plus the same flow without U1 (control). */
static void r9_unknown_brownout_flow(bool u1)
{
    fresh_nvs(); reboot(ESP_RST_POWERON, 1, true);
    reboot(ESP_RST_BROWNOUT, 0, false);                  /* undated, boot counter unavailable */
    advance_s(7 * H);
    reboot(ESP_RST_BROWNOUT, 2, true);                   /* a later, dated entry */
    CHECK(s_core.p.hist_n == 2, "two recorded brownouts");
    good_reads(READS_30MIN + 4, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL, "healthy evidence recovers");
    advance_s(7 * H);
    if (u1) g_epoch_get_err = ESP_FAIL;
    reboot(ESP_RST_BROWNOUT, 0, false);                  /* a NEW unknown-identity brownout */
    CHECK(s_core.p.hist_n == 3, "recorded (%u)", (unsigned)s_core.p.hist_n);
    g_epoch_get_err = ESP_OK;
    CHECK(power_protect_force(PP_STATE_NO_FLASH) == ESP_OK, "verify (U2) and force down");
    CHECK(s_core.p.hist_n == 3 && s_core.p.hist[2].utc == 0 && s_core.p.hist[2].boot == 0,
          "the new unknown entry is carried, newest last (hist %u)", (unsigned)s_core.p.hist_n);
    advance_s(60);
    reboot(ESP_RST_BROWNOUT, 4, true);
    CHECK(st() == PP_STATE_HOLD, "the next brownout escalates (state %d)", st());
}
static void r9_u1_unknown_brownout_dedup(void) { r9_unknown_brownout_flow(true); }
static void r9_unknown_brownout_no_u1_control(void) { r9_unknown_brownout_flow(false); }
#endif

static void low_voltage_trigger(void)
{
    reboot(ESP_RST_POWERON, 1, true);
    read5(POWER_READ_OK, 3200, 3900, true, true);   /* first frame: no freshness yet */
    CHECK(st() == PP_STATE_NORMAL, "a single frame is not fresh evidence");
    read5(POWER_READ_OK, 3200, 3900, true, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.p.reason == PP_REASON_LOW_VOLTAGE, "rail < 3.30 V");
    CHECK(strcmp(g_last_e, "power_protect: normal -> no_flash reason=low_voltage") == 0, "%s", g_last_e);

    fresh_nvs();
    reboot(ESP_RST_POWERON, 5, true);
    good_reads(3, 4100, 3350);
    CHECK(st() == PP_STATE_NO_FLASH, "0 < battery < 3.40 V");

    /* Never on stale, invalid, no_input or input-absent reads. */
    fresh_nvs();
    g_writes = 0;
    reboot(ESP_RST_POWERON, 6, true);
    for (int i = 0; i < 30; i++) read5(POWER_READ_OK, 3000, 3000, true, false);   /* frozen */
    read5(POWER_READ_NO_INPUT, 0, 0, false, true);
    read5(POWER_READ_IMPLAUSIBLE, 0, 0, false, true);
    for (int i = 0; i < 5; i++) read5(POWER_READ_OK, 3000, 3000, false, true);    /* on battery */
    CHECK(st() == PP_STATE_NORMAL && g_writes == 1, "no trigger without fresh present evidence (first record only)");
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
    CHECK(pp_encode(&p, b, sizeof b) == PP_BLOB_LEN && PP_BLOB_LEN == 128, "len");
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
    fresh_nvs();
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
    /* forced hold, one stored byte flipped. A reset WITHOUT power loss finds
     * the record in the RTC mirror (newer than an unreadable NVS) and puts it
     * back on record at once. */
    reboot(ESP_RST_POWERON, 1, true);
    CHECK(power_protect_force(PP_STATE_HOLD) == ESP_OK, "force");
    g_blob[20] ^= 0x10;
#if HAVE_GEN
    const int w0 = g_writes;
    reboot(ESP_RST_SW, 2, true);
    CHECK(st() == PP_STATE_HOLD && s_core.p.forced && !s_core.persist_fault && g_writes == w0 + 1,
          "RTC mirror restores and repairs the forced hold (state %d)", st());
    g_blob[20] ^= 0x10;
#endif
    /* After a power loss the mirror is gone: unreadable = no_flash. */
    const int w = g_writes;
    reboot(ESP_RST_POWERON, 2, true);
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
    /* Still unreadable on the next power-on (not repaired yet): still protected. */
    reboot(ESP_RST_POWERON, 3, true);
    CHECK(st() == PP_STATE_NO_FLASH && g_writes == w, "no write until a transition");
    /* Exits through accumulated evidence; that transition repairs NVS. */
    good_reads(READS_30MIN + 2, 4100, 3900);
    CHECK(st() == PP_STATE_NORMAL && g_writes == w + 1 && !s_core.persist_fault, "recovered + repaired");
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_NORMAL && !s_core.persist_fault, "repaired blob reads back");
    /* ... or through clear. */
    g_blob[30] ^= 1;
    reboot(ESP_RST_POWERON, 5, true);
    CHECK(st() == PP_STATE_NO_FLASH && power_protect_clear() == ESP_OK && st() == PP_STATE_NORMAL &&
          !s_core.persist_fault, "clear repairs");
    /* A stored blob of the wrong size is unreadable too. */
    g_blob_len = PP_BLOB_LEN - 1;
    reboot(ESP_RST_POWERON, 6, true);
    CHECK(st() == PP_STATE_NO_FLASH && s_core.persist_fault, "short blob");
}

static void read_failures_are_protected(void)
{
    /* A previously protected unit whose NVS read fails, open or get. */
    reboot(ESP_RST_BROWNOUT, 2, true);
    reboot(ESP_RST_BROWNOUT, 3, true);
    CHECK(st() == PP_STATE_HOLD, "hold stored");
    g_get_err = ESP_FAIL;
#if HAVE_GEN
    reboot(ESP_RST_SW, 4, true);
    CHECK(st() == PP_STATE_NO_FLASH, "state and epoch unreadable: U1, fail-closed no_flash (state %d)", st());
#endif
    reboot(ESP_RST_POWERON, 4, true);
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
    fresh_nvs();
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
        fresh_nvs();
        reboot(ESP_RST_BROWNOUT, k == 0 ? 0 : 7, false);
        CHECK(st() == PP_STATE_NO_FLASH, "k=%d first", k);
        reboot(ESP_RST_BROWNOUT, k == 0 ? 5 : 0, false);
        CHECK(st() == PP_STATE_HOLD && s_core.p.reason == PP_REASON_BROWNOUT_UNORDERED, "k=%d escalates", k);
        power_protect_get_status(&s);
        CHECK(s.brownouts_6h == 2, "k=%d count %u", k, (unsigned)s.brownouts_6h);
    }
    /* A valid clock still orders brownouts with a counter of 0: UTC decides. */
    fresh_nvs();
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
    fresh_nvs();
    reboot(ESP_RST_BROWNOUT, 3, true);
    quiet_reads(POWER_READ_NO_INPUT, 6 * H + 60);
    CHECK(power_protect_render_json(json, sizeof json) > 0, "render");
    printf("JSON_PROBATION %s\n", json);
    fresh_nvs();
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
    CHECK(st() == PP_STATE_NORMAL && g_writes == 1, "nothing changed (first record only)");
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
#if HAVE_RECOVERY
    {"rtc_repeated_resets", rtc_repeated_resets},
    {"reset_at_commit_boundary", reset_at_commit_boundary},
    {"rejected_relaxation_keeps_low", rejected_relaxation_keeps_low},
    {"low_clock_trust_change", low_clock_trust_change},
    {"operator_relaxation_needs_record", operator_relaxation_needs_record},
    {"rtc_store_interrupted", rtc_store_interrupted},
    {"relaxation_write_then_error", relaxation_write_then_error},
    {"rtc_bound_to_nvs_epoch", rtc_bound_to_nvs_epoch},
    {"relaxation_reread_inconclusive", relaxation_reread_inconclusive},
    {"epoch_missing_first_boot", epoch_missing_first_boot},
    {"no_epoch_no_relaxation", no_epoch_no_relaxation},
    {"fence_unconfirmed", fence_unconfirmed},
    {"missing_record_fails_closed", missing_record_fails_closed},
    {"u1_unverified_is_passive", u1_unverified_is_passive},
    {"u2_verification_reconciles", u2_verification_reconciles},
    {"same_epoch_low_veto", same_epoch_low_veto},
    {"slot_shadow", slot_shadow},
    {"r7_probation_merge", r7_probation_merge},
    {"r7_history_merge", r7_history_merge},
    {"r7_clear_reverify", r7_clear_reverify},
    {"r7_verify_rtc_interrupt", r7_verify_rtc_interrupt},
    {"r7_probation_natural", r7_probation_natural},
    {"r7_history_natural", r7_history_natural},
    {"r7_clear_unreadable", r7_clear_unreadable},
    {"r7_verify_rtc_interrupt_hold", r7_verify_rtc_interrupt_hold},
    {"r7_clear_reply", r7_clear_reply},
    {"r7_merge_gen", r7_merge_gen},
    {"r7_since_merge", r7_since_merge},
    {"r7_merge_gen_natural", r7_merge_gen_natural},
    {"r8_u1_brownout_auto_verification", r8_u1_brownout_auto_verification},
    {"r8_u1_brownout_survives_verification", r8_u1_brownout_survives_verification},
    {"r8_u1_force_survives_verification", r8_u1_force_survives_verification},
    {"r8_u1_u2_cycles", r8_u1_u2_cycles},
    {"r8_u1_brownout_history_cap", r8_u1_brownout_history_cap},
    {"r9_u1_unknown_brownout_dedup", r9_u1_unknown_brownout_dedup},
    {"r9_unknown_brownout_no_u1_control", r9_unknown_brownout_no_u1_control},
    {"cmd_persist_unconfirmed", cmd_persist_unconfirmed},
#endif
#ifdef PP_NATIVE_NVS
    {"native_relaxation_matrix", native_relaxation_matrix},
    {"native_two_fault_scan", native_two_fault_scan},
#endif
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
