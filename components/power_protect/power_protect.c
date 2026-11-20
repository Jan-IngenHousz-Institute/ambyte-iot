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
 * record, as a fence after an unproven relaxation outcome (until two clean
 * writes follow the last failure, at most four), and on a first install the
 * first record and then (key "epoch") the epoch, once per NVS lifetime. Never per read, never per heartbeat: a flash write
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
/* The fence (commit_relaxation): PP_FENCE_RUN clean writes after the last
 * failure, at most PP_FENCE_WRITES writes in all. */
#define PP_FENCE_RUN    2
#define PP_FENCE_WRITES 4
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
    /* Written while the epoch was UNVERIFIED (round 6): its generation belongs
     * to no known sequence, so it is never ORDERED against anything; its
     * protective facts are FOLDED in (pp_merge_protective) until NVS holds
     * them, then the slot is retired. */
    uint32_t unverified;
    uint8_t  blob[PP_BLOB_LEN];
} pp_rtc_slot_t;
static RTC_NOINIT_ATTR pp_rtc_slot_t s_rtc[2];
#define PP_RTC_TAGGED 1   /* (host tests: this build tags unverified RTC writes) */
static uint32_t s_epoch;
/* s_epoch was READ from NVS (or created and read back) this boot. False when
 * it was borrowed from an RTC record because the key was unreadable: then RTC
 * generations cannot be compared with NVS ones (another lifetime may have
 * written either), records are merged instead of selected, and no automatic
 * relaxation runs (rounds 5-6). */
static bool     s_epoch_verified;

/* Any slot that decodes, whatever its epoch or tag. */
static bool rtc_decode(int i, pp_persist_t *out)
{
    return s_rtc[i].magic == PP_RTC_MAGIC && pp_decode(out, s_rtc[i].blob, sizeof s_rtc[i].blob);
}

/* A slot of the VERIFIED lifetime, written while verified: orderable by gen. */
static bool rtc_slot(int i, pp_persist_t *out)
{
    return s_epoch_verified && s_epoch != 0 && s_rtc[i].epoch == s_epoch && s_rtc[i].unverified == 0 &&
           rtc_decode(i, out);
}

/* A tagged (unverified) slot, any epoch: its facts are folded, never ordered. */
static bool rtc_tagged(int i, pp_persist_t *out)
{
    return s_rtc[i].unverified != 0 && rtc_decode(i, out);
}

/* The newest orderable slot; -1 when none. */
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

/* The slot a rewrite must NOT touch: verified, the newest orderable one;
 * unverified, the decodable one with the higher generation (both were merged
 * into the state at boot, so either may go, but never both at once). */
static int rtc_anchor(void)
{
    pp_persist_t p, best_p;
    int best = -1;
    for (int i = 0; i < 2; i++) {
        const bool ok = s_epoch_verified ? rtc_slot(i, &p) : rtc_decode(i, &p);
        if (ok && (best < 0 || p.gen > best_p.gen)) {
            best_p = p;
            best = i;
        }
    }
    return best;
}

static void rtc_store(const pp_persist_t *p)
{
    if (s_epoch_verified ? s_epoch == 0 : false) return;
    uint8_t enc[PP_BLOB_LEN];
    if (pp_encode(p, enc, sizeof enc) != PP_BLOB_LEN) return;
    const uint32_t tag = s_epoch_verified ? 0 : 1;
    const int n = rtc_anchor();
    if (n >= 0 && s_rtc[n].unverified == tag && s_rtc[n].epoch == s_epoch &&
        memcmp(s_rtc[n].blob, enc, sizeof enc) == 0) {
        return;   /* already the newest */
    }
    const int t = n < 0 ? 0 : 1 - n;   /* never the slot holding the newest record */
    PP_TEST_POINT(1);
    s_rtc[t].magic = 0;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);   /* invalidate before rewriting */
    PP_TEST_POINT(2);
    s_rtc[t].epoch = s_epoch;
    s_rtc[t].unverified = tag;
    memcpy(s_rtc[t].blob, enc, sizeof enc);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);   /* arm only after the record is complete */
    PP_TEST_POINT(3);
    s_rtc[t].magic = PP_RTC_MAGIC;
}

/* NVS now holds a record that includes every tagged slot's facts (or one an
 * operator wrote over them): retire the tagged slots, so they are not folded
 * in again at the next boot. Verified only. */
static void rtc_retire_tagged(void)
{
    if (!s_epoch_verified) return;
    for (int i = 0; i < 2; i++) {
        if (s_rtc[i].unverified != 0) s_rtc[i].magic = 0;
    }
}

/* Records equal in every protective fact (generation ignored)? */
static bool same_facts(const pp_persist_t *a, const pp_persist_t *b)
{
    pp_persist_t x = *a, y = *b;
    x.gen = y.gen = 0;
    uint8_t ea[PP_BLOB_LEN], eb[PP_BLOB_LEN];
    return pp_encode(&x, ea, sizeof ea) == PP_BLOB_LEN && pp_encode(&y, eb, sizeof eb) == PP_BLOB_LEN &&
           memcmp(ea, eb, sizeof ea) == 0;
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

static void rtc_store(const pp_persist_t *p);

/* Under s_mtx. A missing or unverified epoch leaves the RTC mirror without a
 * reset-safe identity: a failure NVS refused could not be told apart from a
 * foreign record at the next boot (rounds 4-5: a first boot that could not
 * create the epoch, then a probe that NVS took and a failure it refused, came
 * back at full load after a software reset). So: read it again whenever NVS
 * has just taken a write and before every automatic relaxation; create it
 * when absent (a record exists by then); once verified, re-arm the mirror
 * under it. True = verified. */
static bool epoch_ensure(void)
{
    if (s_epoch_verified) return true;
    uint32_t e = 0;
    const int r = epoch_read(&e);
    if (r == 0) e = epoch_create();
    if ((r == 1 || r == 0) && e != 0) {
        s_epoch = e;
        s_epoch_verified = true;
        /* Reconcile BEFORE any relaxation (round 6): the state already holds
         * the composite of every candidate merged at boot; fold any tagged
         * slot written since, put the result on NVS (a newer generation than
         * anything there) and only then retire the tagged slots. */
        pp_persist_t t;
        for (int i = 0; i < 2; i++) {
            if (rtc_tagged(i, &t)) pp_merge_protective(&s_core.p, &t);
        }
        pp_persist_t durable;
        const char *why = NULL;
        const bool readable = nvs_record(&durable, &why) == REC_OK;
        if (!readable || !same_facts(&durable, &s_core.p)) {
            if (readable && durable.gen > s_core.p.gen) s_core.p.gen = durable.gen;
            s_core.p.gen++;
            if (persist(&s_core.p) == ESP_OK) {
                s_core.persist_fault = false;
                rtc_retire_tagged();
            } else {
                s_core.persist_fault = true;
            }
        } else {
            rtc_retire_tagged();
        }
        ESP_LOGW(TAG, "NVS epoch verified: state reconciled, RTC mirror armed");
        rtc_store(&s_core.p);
    }
    return s_epoch_verified;
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
    rtc_retire_tagged();    /* NVS holds the current record (verified only) */
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
    /* Written until a RUN of PP_FENCE_RUN clean writes follows the LAST
     * failure, at most PP_FENCE_WRITES writes. The native IDF matrix found a
     * write that failed part-way leaving a stray chunk under the next blob
     * version while this boot's Storage still holds the old index: the next
     * clean write lands under that same version and after a re-init the stray
     * RELAXED record is what NVS returns (round 4). A second clean write
     * toggles the version and its eraseMultiPageBlob of the previous version
     * removes the strays. And a failed fence write is just such a part-way
     * write again: a clean write after a failed FENCE does not cancel it
     * (round 5, native two-fault repro: faults on raw writes #2 and #4 ->
     * "confirmed" after one clean fence, NO record after a re-init). Only a
     * clean write counts: a positive read proves nothing about strays. */
    int run = 0;
    for (int n = 0; n < PP_FENCE_WRITES && run < PP_FENCE_RUN; n++) {
        s_core.p.gen = (n == 0 ? s_cand.p.gen : s_core.p.gen) + 1;
        rtc_store(&s_core.p);
        run = persist(&s_core.p) == ESP_OK ? run + 1 : 0;
    }
    const bool fenced = run >= PP_FENCE_RUN;
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
    uint32_t e = 0;
    const int er = epoch_read(&e);
    s_epoch = er == 1 ? e : 0;
    s_epoch_verified = er == 1;
    if (er < 0 && !power_lost) {
        /* Present but unreadable: no proof of a rewrite, so the RTC record's
         * own epoch is borrowed, UNVERIFIED (see the adoption rule below). */
        s_epoch = rtc_any_epoch();
    }

    /* The NVS record. ABSENT with no epoch = a first install (or a wiped NVS):
     * normal, and that record is written before the epoch, so an epoch always
     * implies a record was written. ABSENT with an epoch (or an unreadable
     * one) = the state was LOST (round 5: an interrupted fence sequence left
     * no record and the unit booted at full load): no_flash, state_missing.
     * UNREADABLE = no_flash, state_unreadable (review of PR #103). */
    const bool first_install = rs == REC_ABSENT && er == 0;
    if (rs == REC_UNREADABLE) {
        ESP_LOGW(TAG, "persisted state unreadable (%s) - starting at no_flash", unreadable);
        pp_load_unreadable(&s_core, clk);
    } else if (rs == REC_ABSENT && !first_install) {
        ESP_LOGW(TAG, "persisted state missing beside an NVS epoch - starting at no_flash");
        pp_load_missing(&s_core, clk);
    }

    /* The RTC mirror only counts after a reset without power loss (after one
     * it is gone or garbage). It is not consumed: until NVS takes a newer
     * record, every such reset adopts it again.
     *  - VERIFIED epoch (same NVS lifetime): the newer orderable record by
     *    generation wins (NVS absent or unreadable loses to it); then the
     *    protective facts of every TAGGED slot (written while unverified) are
     *    folded in.
     *  - UNVERIFIED (borrowed) epoch: nothing can be ordered (round 6: a
     *    same-lifetime low veto was dropped on a rank tie; a foreign gen 17
     *    shadowed an intact hold in the other slot). So NOTHING is selected:
     *    the NVS record (or the fail-closed no_flash) and EVERY decodable slot
     *    are MERGED into the most protective composite (pp_merge_protective),
     *    and it is written to NVS only if it adds protection. */
    bool adopted = false;
    /* An ABSENT epoch (a first install, or an NVS wiped / rewritten since)
     * means no RTC record belongs here at all: none is considered. */
    if (!power_lost && er != 0) {
        pp_persist_t source = s_core.p;   /* NVS, or the fail-closed baseline */
        pp_persist_t cand;
        if (s_epoch_verified && rtc_load(&cand) && (rs != REC_OK || cand.gen > s_core.p.gen)) {
            source = cand;                 /* the newer record of this lifetime */
            adopted = true;
        }
        pp_persist_t merged = source;
        uint32_t top = source.gen > s_core.p.gen ? source.gen : s_core.p.gen;
        for (int i = 0; i < 2; i++) {
            const bool fold = s_epoch_verified ? rtc_tagged(i, &cand) : rtc_decode(i, &cand);
            if (!fold) continue;
            if (cand.gen > top) top = cand.gen;
            pp_merge_protective(&merged, &cand);
        }
        if (!same_facts(&merged, &source)) {
            merged.gen = top + 1;          /* a new record, newer than every candidate */
            adopted = true;
        }
        if (adopted) {
            ESP_LOGW(TAG, "boot record from RTC memory: %s (reason %s, gen %lu; epoch %s)",
                     pp_state_name((pp_state_t)merged.state), pp_reason_name((pp_reason_t)merged.reason),
                     (unsigned long)merged.gen, s_epoch_verified ? "verified" : "unverified, merged");
            pp_core_init(&s_core);
            s_core.p = merged;
            s_core.persist_fault = true;   /* not on record until NVS takes it */
        }
    }
    lock();
    if (adopted) {
        /* Put it on record: a record of THIS lifetime, or a composite that is
         * only ever MORE protective than NVS. Once per boot; on failure the
         * mirror stays armed (and tagged slots stay until NVS has them). */
        if (persist(&s_core.p) == ESP_OK) write_ok();
    } else if (s_epoch_verified && rs == REC_OK) {
        rtc_retire_tagged();   /* nothing in them that NVS lacks */
    }
    if (first_install) {
        /* The first record, then the epoch (each read back). A failed record
         * write leaves both absent: the next boot is a first install again. */
        s_core.p.gen = 1;
        if (persist(&s_core.p) == ESP_OK || nvs_holds(&s_core.p)) {
            s_epoch = epoch_create();
            s_epoch_verified = s_epoch != 0;
        }
    } else if (er == 0) {
        /* A record (or an unreadable one) but no epoch yet: an upgrade from
         * the first release, or a first install whose epoch write failed. */
        s_epoch = epoch_create();
        s_epoch_verified = s_epoch != 0;
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
