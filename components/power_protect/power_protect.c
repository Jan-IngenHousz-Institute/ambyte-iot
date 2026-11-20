/*
 * power_protect.c — target glue for the brownout protection state machine
 * (contract: include/power_protect.h; decisions: power_protect_core.c).
 *
 * NVS namespace "pwr_protect", key "state": the pp_encode blob (128 B, v2; the
 * 113-B v1 blob of the first release is still read, as generation 0). Written
 * ONLY when the core says persist (a state transition, the probe step and a
 * failed probe included; a passed probation; a recorded brownout, at most one
 * per boot; a protected unit's fresh low read, first / dating / at most
 * hourly), on force and clear, once at boot to put an adopted RTC record on
 * record, as a fence after an unreadable relaxation outcome, and (key "epoch")
 * once per NVS lifetime. Never per read, never per heartbeat: a flash write
 * per 5 s charger read would wear the NVS sector for nothing. RTC_NOINIT is not
 * enough on its own: a brownout is a power-on-class reset, exactly the reset
 * this module exists for, and it clears RTC memory.
 *
 * ONE generation-ordered record (review of PR #113, rounds 1-2). Two earlier
 * fixes in a row (an RTC copy consumed at its first adoption; one written only
 * after a refused NVS write) each left a reset window, so the model is now:
 *  - every record written carries pp_persist_t.gen = the previous number + 1,
 *    never reused (a refused relaxation consumes its number too);
 *  - s_rtc mirrors the newest record in RTC_NOINIT memory, in two slots (a
 *    rewrite never touches the newest one) bound to the NVS epoch; it
 *    survives resets WITHOUT power loss and is never cleared by being read;
 *  - a PROTECTIVE update is applied, written to s_rtc, then to NVS;
 *  - a RELAXATION (pp_relax, an operator clear / force down) is computed on a
 *    copy (s_cand), written to NVS FIRST, and only then adopted and mirrored
 *    to s_rtc. A refused write leaves the state as it was after the read's
 *    protective facts, and automatic relaxations wait PP_RELAX_RETRY_MS. An
 *    NVS error is re-read (round 3): the pinned IDF can report one after the
 *    new blob was written; landed = it stands, unreadable = fenced by a newer
 *    protective record;
 *  - at boot the newer valid record by generation wins: s_rtc (only after a
 *    reset without power loss) over NVS when its generation is higher. The
 *    adopted record is written to NVS once (sync), and s_rtc stays armed.
 * Why that closes the windows: s_rtc is only ever AHEAD of NVS by protective
 * updates (relaxations reach NVS first), and a reset between the two writes
 * of either kind leaves the newer record winning: protective -> s_rtc is
 * newer; relaxation -> NVS is newer.
 */
#include "power_protect.h"

#include <string.h>
#include <time.h>

#include "clock_trust.h"
#include "diag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define TAG "power_protect"
#define PP_NVS_NS  "pwr_protect"
#define PP_NVS_KEY "state"
#define PP_NVS_EPOCH "epoch"
/* Host tests inject resets inside the RTC write (tests/power_protect_host). */
#ifndef PP_TEST_POINT
#define PP_TEST_POINT(n) ((void)0)
#endif

static SemaphoreHandle_t s_mtx;
static StaticSemaphore_t s_mtx_storage;
static pp_core_t s_core;                 /* under s_mtx */
static pp_core_t s_cand;                 /* under s_mtx: a relaxation's candidate (~0.5 KiB, off the stack) */
/* Published copy of s_core.p.state for lock-free readers (sched_runner,
 * ota_update, the pong on the esp-mqtt task). A byte store is atomic here. */
static volatile uint8_t s_state_pub = PP_STATE_NORMAL;
static portMUX_TYPE s_skip_mux = portMUX_INITIALIZER_UNLOCKED;
static pp_skips_t s_skips;               /* under s_skip_mux */

/* The RTC_NOINIT mirror of the newest record (see the header comment), in
 * TWO slots (review of PR #113, round 3): a rewrite goes to the slot that does
 * NOT hold the newest record, which stays valid until the new one is armed, so
 * a reset inside the write never leaves no valid copy. Each slot is validated
 * by a magic word, the blob's own CRC and the NVS epoch (below); garbage after
 * a power-on never decodes.
 *
 * The NVS epoch binds RTC records to one NVS lifetime: a random non-zero u32
 * kept in NVS ("epoch"), created once when absent. A deliberate NVS rewrite or
 * wipe (flash_gui provisioning, clean_nvs) creates a new one, so a record left
 * in RTC memory from before it is never adopted. If the key exists but cannot
 * be READ, nothing proves a rewrite: the epoch of the newest valid RTC record
 * is taken (an NVS read fault must not cost the retained protection). 0 = no
 * epoch at all (absent and not creatable, or unreadable with no RTC record):
 * RTC records are then neither written nor adopted. */
#define PP_RTC_MAGIC 0x50505247u   /* "PPRG" */
typedef struct {
    volatile uint32_t magic;
    uint32_t epoch;
    uint8_t  blob[PP_BLOB_LEN];
} pp_rtc_slot_t;
static RTC_NOINIT_ATTR pp_rtc_slot_t s_rtc[2];
static uint32_t s_epoch;

static bool rtc_slot(int i, pp_persist_t *out)
{
    return s_epoch != 0 && s_rtc[i].magic == PP_RTC_MAGIC && s_rtc[i].epoch == s_epoch &&
           pp_decode(out, s_rtc[i].blob, sizeof s_rtc[i].blob);
}

/* The valid slot with the higher generation; -1 when none. */
static int rtc_newest(pp_persist_t *out)
{
    int best = -1;
    pp_persist_t p;
    for (int i = 0; i < 2; i++) {
        if (rtc_slot(i, &p) && (best < 0 || p.gen > out->gen)) {
            *out = p;
            best = i;
        }
    }
    return best;
}

static bool rtc_load(pp_persist_t *out)
{
    return rtc_newest(out) >= 0;
}

static void rtc_store(const pp_persist_t *p)
{
    if (s_epoch == 0) return;
    uint8_t enc[PP_BLOB_LEN];
    if (pp_encode(p, enc, sizeof enc) != PP_BLOB_LEN) return;
    pp_persist_t cur;
    const int n = rtc_newest(&cur);
    if (n >= 0 && memcmp(s_rtc[n].blob, enc, sizeof enc) == 0) return;   /* already the newest */
    const int t = n < 0 ? 0 : 1 - n;   /* never the slot holding the newest record */
    PP_TEST_POINT(1);
    s_rtc[t].magic = 0;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);   /* invalidate before rewriting */
    PP_TEST_POINT(2);
    s_rtc[t].epoch = s_epoch;
    memcpy(s_rtc[t].blob, enc, sizeof enc);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);   /* arm only after the record is complete */
    PP_TEST_POINT(3);
    s_rtc[t].magic = PP_RTC_MAGIC;
}

/* Valid = a UTC the brownout history may compare across boots. SNTP outranks
 * everything. An RTC-sourced clock counts only when nothing made it suspect
 * (OSF / read disagreement this boot) and it is past the 2024 floor. A clock
 * held up by the persisted high-water mark ("hwm", after a power loss stopped
 * the RTC) is a lower bound, not the time: never valid here. */
static pp_clock_t clock_now(void)
{
    const char *src = "rtc";
    bool suspect = false;
    clock_trust_get_status(&src, &suspect);
    const time_t now = time(NULL);
    const bool plausible = (long long)now >= CLOCK_TRUST_VALID_FLOOR_S;
    const bool trusted = strcmp(src, "sntp") == 0 || (strcmp(src, "rtc") == 0 && !suspect);
    return (pp_clock_t){ .valid = plausible && trusted, .utc = (int64_t)now };
}

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t persist(const pp_persist_t *p)
{
    uint8_t blob[PP_BLOB_LEN];
    if (pp_encode(p, blob, sizeof blob) != PP_BLOB_LEN) return ESP_FAIL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(PP_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, PP_NVS_KEY, blob, sizeof blob);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

typedef enum { REC_OK, REC_ABSENT, REC_UNREADABLE } rec_status_t;

/* The record NVS holds now. *why names an unreadable one. */
static rec_status_t nvs_record(pp_persist_t *out, const char **why)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PP_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return REC_ABSENT;
    if (err != ESP_OK) {
        *why = esp_err_to_name(err);
        return REC_UNREADABLE;
    }
    uint8_t blob[PP_BLOB_LEN];
    size_t len = sizeof blob;
    err = nvs_get_blob(h, PP_NVS_KEY, blob, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return REC_ABSENT;
    if (err != ESP_OK) {
        *why = esp_err_to_name(err);
        return REC_UNREADABLE;
    }
    if (!pp_decode(out, blob, len)) {
        *why = "corrupt";
        return REC_UNREADABLE;
    }
    return REC_OK;
}

/* The epoch of the newest valid RTC record, whatever epoch it carries. */
static uint32_t rtc_any_epoch(void)
{
    uint32_t e = 0, gen = 0;
    pp_persist_t p;
    for (int i = 0; i < 2; i++) {
        if (s_rtc[i].magic == PP_RTC_MAGIC && s_rtc[i].epoch != 0 &&
            pp_decode(&p, s_rtc[i].blob, sizeof s_rtc[i].blob) && (e == 0 || p.gen > gen)) {
            e = s_rtc[i].epoch;
            gen = p.gen;
        }
    }
    return e;
}

/* The epoch key: 1 = read (*out), 0 = absent, -1 = present but unreadable. */
static int epoch_read(uint32_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PP_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u32(h, PP_NVS_EPOCH, out);
        nvs_close(h);
        if (err == ESP_OK && *out != 0) return 1;
    }
    return err == ESP_ERR_NVS_NOT_FOUND ? 0 : -1;
}

/* A new epoch, one write per NVS lifetime. Like every write here, a reported
 * error is not proof that nothing landed (round 4 of the PR #113 review):
 * whatever the write says, the epoch is what reads back. 0 = none. */
static uint32_t epoch_create(void)
{
    uint32_t e;
    do { e = esp_random(); } while (e == 0);
    nvs_handle_t h;
    if (nvs_open(PP_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, PP_NVS_EPOCH, e) == ESP_OK) (void)nvs_commit(h);
        nvs_close(h);
    }
    uint32_t back = 0;
    return epoch_read(&back) == 1 ? back : 0;
}

/* The NVS epoch at boot (see s_rtc). */
static uint32_t epoch_load(bool power_lost)
{
    uint32_t e = 0;
    const int r = epoch_read(&e);
    if (r == 1) return e;
    /* Present but unreadable: not a rewrite. Keep the RTC record's own. */
    if (r < 0) return power_lost ? 0 : rtc_any_epoch();
    return epoch_create();   /* absent: a new NVS, or one rewritten / wiped since */
}

static void rtc_store(const pp_persist_t *p);

/* Under s_mtx. Without an epoch the RTC mirror is off, so nothing survives a
 * reset that NVS refused (round 4: a first boot that could not create the
 * epoch, then a probe that NVS took and a failure it refused, came back at
 * full load after a software reset). So: try again whenever NVS has just
 * taken a write and before every automatic relaxation; once it exists, arm
 * the mirror with the current record. */
static bool epoch_ensure(void)
{
    if (s_epoch != 0) return true;
    uint32_t e = 0;
    const int r = epoch_read(&e);
    s_epoch = r == 1 ? e : r == 0 ? epoch_create() : 0;
    if (s_epoch != 0) {
        ESP_LOGW(TAG, "NVS epoch established: RTC mirror armed");
        rtc_store(&s_core.p);
    }
    return s_epoch != 0;
}

/* Under s_mtx. One ESP_LOGE per state transition: v2.18.0's diag hook turns
 * it into an ambyte.log/1 record, so the cloud sees every change with its
 * reason even when no heartbeat lands in between. */
static void publish(const pp_event_t *ev)
{
    s_state_pub = s_core.p.state;
    if (ev->changed) {
        ESP_LOGE(TAG, "power_protect: %s -> %s reason=%s", pp_state_name(ev->from),
                 pp_state_name(ev->to), pp_reason_name(ev->reason));
    } else if (ev->probe_passed) {
        /* Not a state change, but the end of a probe: one line, as distinct
         * as the transitions (the cloud sees every probe step and outcome). */
        ESP_LOGE(TAG, "power_protect: %s probation passed reason=probe_passed backoff_level=%u",
                 pp_state_name(ev->to), (unsigned)s_core.p.probe_fails);
    }
}

static void write_ok(void)
{
    s_core.relax_hold_until_ms = 0;
    if (s_core.persist_fault) {
        ESP_LOGW(TAG, "persisted state repaired");
        s_core.persist_fault = false;
    }
    (void)epoch_ensure();   /* NVS takes writes again: a missing epoch now */
}

/* Under s_mtx. A PROTECTIVE (or neutral) event already applied to s_core:
 * publish at once, then the next generation to the RTC mirror, then NVS. */
static esp_err_t apply(const pp_event_t *ev)
{
    publish(ev);
    if (!ev->persist) return ESP_OK;
    s_core.p.gen++;
    rtc_store(&s_core.p);
    const esp_err_t err = persist(&s_core.p);
    if (err != ESP_OK) {
        /* RAM and the RTC mirror hold the new record; every reset without
         * power loss adopts it until NVS takes a newer one. Warn (not error):
         * the transition line already went out. */
        ESP_LOGW(TAG, "state not persisted: %s (kept in RTC memory)", esp_err_to_name(err));
        s_core.persist_fault = true;
    } else {
        write_ok();
    }
    return err;
}

/* Under s_mtx. A RELAXATION computed on s_cand: NVS first, adopted only once
 * on record. A refused write consumes the generation number and leaves s_core
 * (with every protective fact of the read) as it is; an automatic relaxation
 * then waits PP_RELAX_RETRY_MS. */
/* Does NVS, read back now, hold exactly this record? */
static bool nvs_holds(const pp_persist_t *p)
{
    pp_persist_t durable;
    const char *why = NULL;
    if (nvs_record(&durable, &why) != REC_OK) return false;
    uint8_t a[PP_BLOB_LEN], b[PP_BLOB_LEN];
    return pp_encode(&durable, a, sizeof a) == PP_BLOB_LEN && pp_encode(p, b, sizeof b) == PP_BLOB_LEN &&
           memcmp(a, b, sizeof a) == 0;
}

/* Write a record and learn whether it is on NVS: true only on a clean write
 * or a POSITIVE read of exactly this record afterwards. */
static bool persist_confirmed(const pp_persist_t *p, esp_err_t *err_out)
{
    const esp_err_t err = persist(p);
    *err_out = err;
    return err == ESP_OK || nvs_holds(p);
}

static esp_err_t commit_relaxation(const pp_event_t *ev, int64_t now_ms, bool automatic)
{
    s_cand.p.gen = s_core.p.gen + 1;
    /* An NVS error does not mean nothing was written: the pinned IDF's
     * Storage::writeItem writes the new blob first and can fail after it
     * (ESP_ERR_NVS_REMOVE_FAILED erasing the old copy, or any error from
     * eraseMultiPageBlob; round 3). Nor does what an immediate re-read
     * returns decide it: after REMOVE_FAILED the native IDF reads NOT_FOUND
     * (Page::alterEntryState invalidated the page for this boot; reopening a
     * handle does not reset that) or even the OLD record, and the NEW one is
     * read after a re-init (round 4, native repro on the real Storage). So
     * only a POSITIVE read of the new record counts as landed; anything else
     * (absent, old, unreadable) is unknown, and fenced. */
    esp_err_t err;
    if (persist_confirmed(&s_cand.p, &err)) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NVS reported %s, but the record reads back: %s -> %s stands", esp_err_to_name(err),
                     pp_state_name(ev->from), pp_state_name(ev->to));
        }
        s_cand.persist_fault = s_core.persist_fault;
        s_core = s_cand;
        rtc_store(&s_core.p);
        write_ok();
        publish(ev);
        return ESP_OK;
    }
    /* Unknown: FENCE it. A newer protective record (the unchanged state; the
     * relaxation's number is consumed, never reused) to RTC and NVS. Only a
     * fence that is itself confirmed on NVS outranks a relaxation that may
     * have landed at EVERY later boot; a fence only in RTC covers resets
     * without power loss, and after a power loss the landed relaxation can
     * win (POWER_PROTECT_ERR_UNCONFIRMED; documented limit). */
    s_core.p.gen = s_cand.p.gen + 1;
    rtc_store(&s_core.p);
    esp_err_t fence_err;
    bool fenced = persist_confirmed(&s_core.p, &fence_err);
    /* ...twice. The native IDF matrix (tests/power_protect_host/nvs_native)
     * found a write that failed part-way leaving a stray chunk under the NEXT
     * blob version, while this boot's Storage still holds the old index: the
     * first fence is then written under that same version, reports ESP_OK,
     * and after a re-init the stray RELAXED record is what NVS returns. A
     * second write toggles the version, and its eraseMultiPageBlob of the
     * previous version removes the stray chunks with the first fence. Only
     * the second write's outcome confirms anything. */
    s_core.p.gen++;
    rtc_store(&s_core.p);
    fenced = persist_confirmed(&s_core.p, &fence_err);
    s_core.persist_fault = !fenced;
    if (automatic) s_core.relax_hold_until_ms = now_ms + PP_RELAX_RETRY_MS;
    ESP_LOGW(TAG, "%s -> %s (%s) not committed: %s - staying %s, %s%s", pp_state_name(ev->from),
             pp_state_name(ev->to), ev->probe_passed ? "probe_passed" : pp_reason_name(ev->reason),
             esp_err_to_name(err), pp_state_name(ev->from),
             fenced ? "fenced on NVS" : "fence NOT confirmed on NVS (RTC only: a power loss may let it win)",
             automatic ? ", next try in 60 min" : "");
    s_state_pub = s_core.p.state;
    return fenced ? err : POWER_PROTECT_ERR_UNCONFIRMED;
}

static bool lock(void)
{
    return s_mtx != NULL && xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_mtx);
}

void power_protect_boot(void)
{
    if (s_mtx != NULL) return;   /* once per boot */
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_storage);
    pp_core_init(&s_core);

    /* ABSENT (first install, or never protected) = normal. UNREADABLE (a
     * failed open or read, or a blob failing version/length/CRC/range checks)
     * = no_flash + persist_fault (pp_load_unreadable): defaulting to normal let
     * a corrupted forced hold resume full load on a watchdog reboot (review of
     * PR #103). */
    const char *unreadable = NULL;
    const rec_status_t rs = nvs_record(&s_core.p, &unreadable);
    if (rs != REC_OK) pp_core_init(&s_core);
    const pp_clock_t clk = clock_now();

    const esp_reset_reason_t rr = esp_reset_reason();
    const bool brownout = rr == ESP_RST_BROWNOUT;
    const bool power_lost = brownout || rr == ESP_RST_POWERON || rr == ESP_RST_PWR_GLITCH;
    s_epoch = epoch_load(power_lost);
    /* The newer record by generation wins. The RTC mirror only counts after
     * a reset without power loss (after one it is gone or garbage), only for
     * this NVS epoch, and only when NVS holds no record as new: it is then
     * ahead by protective updates NVS refused. It is not consumed: until NVS
     * takes a newer record, every such reset adopts it again. */
    pp_persist_t rtc;
    bool adopted = false;
    if (!power_lost && rtc_load(&rtc) && (rs != REC_OK || rtc.gen > s_core.p.gen)) {
        ESP_LOGW(TAG, "adopting the newer record from RTC memory: %s (reason %s, gen %lu over %lu)",
                 pp_state_name((pp_state_t)rtc.state), pp_reason_name((pp_reason_t)rtc.reason),
                 (unsigned long)rtc.gen, (unsigned long)(rs != REC_OK ? 0 : s_core.p.gen));
        pp_core_init(&s_core);
        s_core.p = rtc;
        s_core.persist_fault = true;   /* not on record until NVS takes it */
        adopted = true;
    } else if (rs == REC_UNREADABLE) {
        ESP_LOGW(TAG, "persisted state unreadable (%s) - starting at no_flash", unreadable);
        pp_load_unreadable(&s_core, clk);
    }
    lock();
    if (adopted) {
        /* Put it on record (same generation: it is the same record). Once per
         * boot; on failure the mirror stays armed. */
        if (persist(&s_core.p) == ESP_OK) write_ok();
    }
    rtc_store(&s_core.p);   /* the mirror holds the current record (a no-op when it already does) */
    if (power_lost && !brownout) {
        /* Power-on / power glitch: a probation in flight failed. (A brownout
         * is decided by pp_on_boot, with its history entry.) */
        const pp_event_t lost = pp_on_power_loss(&s_core, clk);
        (void)apply(&lost);
    }
    const pp_event_t ev = pp_on_boot(&s_core, brownout, diag_boot_count(), clk);
    (void)apply(&ev);
    const uint32_t recent = pp_brownouts_6h(&s_core, clk, diag_boot_count());
    unlock();
    ESP_LOGW(TAG, "boot: state=%s reason=%s forced=%d brownouts_6h=%lu probation=%u backoff_level=%u "
                  "(reset %s, clock %s)",
             pp_state_name((pp_state_t)s_core.p.state), pp_reason_name((pp_reason_t)s_core.p.reason),
             s_core.p.forced ? 1 : 0, (unsigned long)recent, (unsigned)s_core.p.probation,
             (unsigned)s_core.p.probe_fails, brownout ? "brownout" : "other",
             clk.valid ? "valid" : "not valid");
}

void power_protect_on_power_read(power_read_reason_t reason, const power_reading_t *p,
                                 bool fresh_present)
{
    if (!lock()) return;   /* before boot: nothing to protect yet */
    const int64_t now = mono_ms();
    const pp_clock_t clk = clock_now();
    /* The read's protective facts (low veto, evidence, failures) are applied
     * and written first, whatever happens to a relaxation it makes due. */
    const pp_event_t ev = pp_on_read(&s_core, reason, p, fresh_present, now, clk);
    (void)apply(&ev);
    if (ev.relax_due != PP_RELAX_NONE && !epoch_ensure()) {
        /* No reset-safe identity for the RTC mirror: a failure after this
         * relaxation that NVS refused could not survive a reset. Hold it. */
        s_core.relax_hold_until_ms = now + PP_RELAX_RETRY_MS;
        ESP_LOGW(TAG, "relaxation held: no NVS epoch (RTC mirror off), next try in 60 min");
    } else if (ev.relax_due != PP_RELAX_NONE) {
        s_cand = s_core;
        const pp_event_t rel = pp_relax(&s_cand, (pp_relax_t)ev.relax_due, now, clk);
        if (rel.relaxes) (void)commit_relaxation(&rel, now, true);
    }
    unlock();
}

pp_state_t power_protect_state(void)
{
    return (pp_state_t)s_state_pub;
}

bool power_protect_blocks_flash(void)
{
    return pp_blocks_flash(power_protect_state());
}

bool power_protect_blocks_ambit(void)
{
    return pp_blocks_ambit(power_protect_state());
}

void power_protect_note_skip(power_protect_skip_t what)
{
    taskENTER_CRITICAL(&s_skip_mux);
    uint32_t *n = what == POWER_PROTECT_SKIP_FLASH_RUN ? &s_skips.flash_runs
                : what == POWER_PROTECT_SKIP_AMBIT_RUN ? &s_skips.ambit_runs
                : &s_skips.ota;
    if (*n < UINT32_MAX) (*n)++;
    taskEXIT_CRITICAL(&s_skip_mux);
}

static pp_skips_t skips_snapshot(void)
{
    taskENTER_CRITICAL(&s_skip_mux);
    const pp_skips_t s = s_skips;
    taskEXIT_CRITICAL(&s_skip_mux);
    return s;
}

void power_protect_get_status(power_protect_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->exit_in_s = -1;
    out->recovery.next_probe_s = -1;
    out->skipped = skips_snapshot();
    if (!lock()) {
        out->state = PP_STATE_NORMAL;
        out->reason = PP_REASON_NONE;
        return;
    }
    out->state = (pp_state_t)s_core.p.state;
    out->reason = (pp_reason_t)s_core.p.reason;
    out->forced = s_core.p.forced;
    out->since_utc = s_core.p.since_utc;
    out->persist_fault = s_core.persist_fault;
    const pp_clock_t clk = clock_now();
    const int64_t now = mono_ms();
    out->brownouts_6h = pp_brownouts_6h(&s_core, clk, diag_boot_count());
    const int64_t left = pp_exit_remaining_ms(&s_core, now);
    out->exit_in_s = left < 0 ? -1 : (left + 999) / 1000;
    out->recovery = pp_recovery(&s_core, now, clk);
    unlock();
}

int power_protect_render_json(char *buf, size_t cap)
{
    const pp_skips_t skips = skips_snapshot();
    if (!lock()) {
        pp_persist_t normal;
        memset(&normal, 0, sizeof normal);
        return pp_render_json(buf, cap, &normal, false, 0, &skips, NULL);
    }
    const pp_clock_t clk = clock_now();
    const pp_persist_t p = s_core.p;
    const bool fault = s_core.persist_fault;
    const uint32_t recent = pp_brownouts_6h(&s_core, clk, diag_boot_count());
    const pp_recovery_t rec = pp_recovery(&s_core, mono_ms(), clk);
    unlock();
    return pp_render_json(buf, cap, &p, fault, recent, &skips, &rec);
}

esp_err_t power_protect_force(pp_state_t state)
{
    if (state != PP_STATE_NO_FLASH && state != PP_STATE_HOLD) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    /* Run on a copy: forcing a LOWER state (hold -> no_flash) is a relaxation
     * and takes effect only once on record; a refused write leaves the unit
     * where it was (the reply says persist_failed, with that state). Forcing
     * up applies at once, like every protective update. */
    s_cand = s_core;
    const pp_event_t ev = pp_force(&s_cand, state, clock_now());
    if (!ev.changed && ev.persist) {
        /* Same state, now forced: not a transition, but worth a line. */
        ESP_LOGW(TAG, "power_protect: %s now forced", pp_state_name(state));
    }
    esp_err_t err;
    if (ev.relaxes) {
        err = commit_relaxation(&ev, mono_ms(), false);
    } else {
        s_cand.persist_fault = s_core.persist_fault;
        s_core = s_cand;
        err = apply(&ev);
    }
    unlock();
    return err;
}

esp_err_t power_protect_clear(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    /* clear lowers protection: on record first (see power_protect_force). */
    s_cand = s_core;
    const pp_event_t ev = pp_clear(&s_cand, clock_now());
    if (!ev.persist) {
        unlock();
        return ESP_OK;   /* nothing to clear */
    }
    if (!ev.changed) ESP_LOGW(TAG, "power_protect: brownout history cleared");
    const esp_err_t err = commit_relaxation(&ev, mono_ms(), false);
    unlock();
    return err;
}
