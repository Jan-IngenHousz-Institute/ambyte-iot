/*
 * power_protect.c — target glue for the brownout protection state machine
 * (contract: include/power_protect.h; decisions: power_protect_core.c).
 *
 * NVS namespace "pwr_protect", key "state": the pp_encode blob (124 B, v2; the
 * 113-B v1 blob of the first release is still read). Written ONLY when the
 * core says persist — a state transition (the probe step and a failed probe
 * included), a passed probation, a recorded brownout (at most one per boot),
 * a protected unit's fresh low read (first, then at most hourly), force,
 * clear. Never per read, never per heartbeat:
 * a flash write per 5 s charger read would wear the NVS sector for nothing.
 * RTC_NOINIT is not enough on its own: a brownout is a power-on-class reset,
 * exactly the reset this module exists for, and it clears RTC memory.
 *
 * Commit order (review of PR #113, round 1):
 *  - A RELAXATION (ev.relaxes: the probe step, a passed probation, the
 *    accumulated exit) is written FIRST and only then published. If the write
 *    fails the core is rolled back to its state before the read, the unit
 *    stays protected and no relaxation is tried for PP_RELAX_RETRY_MS. A probe
 *    that is not on record would come back after a brownout as an ordinary
 *    episode: its failure would never count and the 6-failure cap and the
 *    backoff could be bypassed for ever.
 *  - Everything else (more protection) is published FIRST, then written: the
 *    unit is protected at once even when the write fails. Such an unwritten
 *    state is also kept in an RTC_NOINIT copy (s_unwritten), which a reset
 *    WITHOUT power loss (the nightly reboot, a watchdog, a panic) adopts at
 *    boot, so a failure on probation that NVS refused cannot be undone by a
 *    software reset bringing back the stored probation. After a power loss
 *    the copy is gone, and a stored probation counts as failed instead
 *    (pp_on_power_loss / pp_on_boot).
 */
#include "power_protect.h"

#include <string.h>
#include <time.h>

#include "clock_trust.h"
#include "diag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define TAG "power_protect"
#define PP_NVS_NS  "pwr_protect"
#define PP_NVS_KEY "state"

static SemaphoreHandle_t s_mtx;
static StaticSemaphore_t s_mtx_storage;
static pp_core_t s_core;                 /* under s_mtx */
static pp_core_t s_before;               /* under s_mtx: rollback copy for a relaxation (~0.5 KiB, off the stack) */
/* Published copy of s_core.p.state for lock-free readers (sched_runner,
 * ota_update, the pong on the esp-mqtt task). A byte store is atomic here. */
static volatile uint8_t s_state_pub = PP_STATE_NORMAL;
static portMUX_TYPE s_skip_mux = portMUX_INITIALIZER_UNLOCKED;
static pp_skips_t s_skips;               /* under s_skip_mux */

/* The newest state NVS refused (see the header comment). Survives resets
 * without power loss; validated by a magic word and the blob's own CRC, so
 * the garbage RTC memory holds after a power-on never decodes. */
#define PP_UNWRITTEN_MAGIC 0x50505257u   /* "PPRW" */
typedef struct {
    uint32_t magic;
    uint8_t  blob[PP_BLOB_LEN];
} pp_unwritten_t;
static RTC_NOINIT_ATTR pp_unwritten_t s_unwritten;

static void unwritten_keep(const pp_persist_t *p)
{
    if (pp_encode(p, s_unwritten.blob, sizeof s_unwritten.blob) == PP_BLOB_LEN) {
        s_unwritten.magic = PP_UNWRITTEN_MAGIC;
    } else {
        s_unwritten.magic = 0;
    }
}

static void unwritten_drop(void)
{
    s_unwritten.magic = 0;
}

static bool unwritten_take(pp_persist_t *out)
{
    const bool ok = s_unwritten.magic == PP_UNWRITTEN_MAGIC &&
                    pp_decode(out, s_unwritten.blob, sizeof s_unwritten.blob);
    unwritten_drop();
    return ok;
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

/* Under s_mtx. One ESP_LOGE per state transition: v2.18.0's diag hook turns
 * it into an ambyte.log/1 record, so the cloud sees every change with its
 * reason even when no heartbeat lands in between. */
static esp_err_t apply(const pp_event_t *ev)
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
    if (!ev->persist) return ESP_OK;
    const esp_err_t err = persist(&s_core.p);
    if (err != ESP_OK) {
        /* RAM keeps the new state for this boot, and the RTC copy keeps it
         * across a reset without power loss. Warn (not error): the
         * transition line already went out. health.power_protect.persist_fault
         * says so until a write succeeds. */
        ESP_LOGW(TAG, "state not persisted: %s", esp_err_to_name(err));
        s_core.persist_fault = true;
        unwritten_keep(&s_core.p);
    } else {
        unwritten_drop();
        s_core.relax_hold_until_ms = 0;
        if (s_core.persist_fault) {
            ESP_LOGW(TAG, "persisted state repaired");
            s_core.persist_fault = false;
        }
    }
    return err;
}

/* Under s_mtx. A relaxation: written first, published only once it is on
 * record. On a failed write the core goes back to `before` (this read never
 * happened), the unit stays protected, and relaxations wait. */
static void commit_relaxation(const pp_event_t *ev, const pp_core_t *before, int64_t now_ms)
{
    const esp_err_t err = persist(&s_core.p);
    if (err != ESP_OK) {
        s_core = *before;
        s_core.persist_fault = true;
        s_core.relax_hold_until_ms = now_ms + PP_RELAX_RETRY_MS;
        ESP_LOGW(TAG, "%s -> %s (%s) not committed: %s - staying %s, next try in %lld min",
                 pp_state_name(ev->from), pp_state_name(ev->to), ev->probe_passed ? "probe_passed"
                 : pp_reason_name(ev->reason), esp_err_to_name(err), pp_state_name(ev->from),
                 (long long)(PP_RELAX_RETRY_MS / 60000));
        s_state_pub = s_core.p.state;
        return;
    }
    unwritten_drop();
    s_core.relax_hold_until_ms = 0;
    if (s_core.persist_fault) {
        ESP_LOGW(TAG, "persisted state repaired");
        s_core.persist_fault = false;
    }
    pp_event_t published = *ev;
    published.persist = false;   /* already written */
    (void)apply(&published);
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
     * PR #103). Nothing is written here; the next persisting event repairs it. */
    nvs_handle_t h;
    esp_err_t err = nvs_open(PP_NVS_NS, NVS_READONLY, &h);
    const char *unreadable = NULL;
    if (err == ESP_OK) {
        uint8_t blob[PP_BLOB_LEN];
        size_t len = sizeof blob;
        err = nvs_get_blob(h, PP_NVS_KEY, blob, &len);
        nvs_close(h);
        if (err == ESP_OK && !pp_decode(&s_core.p, blob, len)) {
            unreadable = "corrupt";
        } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            unreadable = esp_err_to_name(err);
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        unreadable = esp_err_to_name(err);
    }
    const pp_clock_t clk = clock_now();
    if (unreadable != NULL) {
        ESP_LOGW(TAG, "persisted state unreadable (%s) - starting at no_flash", unreadable);
        pp_load_unreadable(&s_core, clk);
    }

    const esp_reset_reason_t rr = esp_reset_reason();
    const bool brownout = rr == ESP_RST_BROWNOUT;
    const bool power_lost = brownout || rr == ESP_RST_POWERON || rr == ESP_RST_PWR_GLITCH;
    /* A state NVS refused, kept in RTC memory: newer than the blob. Only a
     * reset without power loss keeps it; otherwise it is dropped unread. */
    pp_persist_t unwritten;
    if (!power_lost && unwritten_take(&unwritten)) {
        ESP_LOGW(TAG, "adopting the unwritten state %s (reason %s) from before the reset",
                 pp_state_name((pp_state_t)unwritten.state), pp_reason_name((pp_reason_t)unwritten.reason));
        s_core.p = unwritten;
        s_core.persist_fault = true;   /* not on record until the next write */
    } else {
        unwritten_drop();
    }
    lock();
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
    s_before = s_core;
    const pp_event_t ev = pp_on_read(&s_core, reason, p, fresh_present, now, clock_now());
    if (ev.relaxes) {
        commit_relaxation(&ev, &s_before, now);
    } else {
        (void)apply(&ev);
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
    const pp_event_t ev = pp_force(&s_core, state, clock_now());
    if (!ev.changed && ev.persist) {
        /* Same state, now forced: not a transition, but worth a line. */
        ESP_LOGW(TAG, "power_protect: %s now forced", pp_state_name(state));
    }
    const esp_err_t err = apply(&ev);
    unlock();
    return err;
}

esp_err_t power_protect_clear(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    const pp_event_t ev = pp_clear(&s_core, clock_now());
    if (!ev.changed && ev.persist) {
        ESP_LOGW(TAG, "power_protect: brownout history cleared");
    }
    const esp_err_t err = apply(&ev);
    unlock();
    return err;
}
