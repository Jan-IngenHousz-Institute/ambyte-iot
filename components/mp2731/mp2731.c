#include "mp2731.h"

#include <string.h>

#include "driver/i2c.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "mp2731_core.h"
#include "mp2731_diagnostics.h"
#include "mp2731_redetect.h"
#include "mp2731_aico.h"

#define TAG "mp2731"

/* Register map, datasheet citations and the read algorithm live in
 * mp2731_core.{h,c} (host-testable); this file is only the I2C adapter. */
#define MP2731_REG_VIN_LIMIT 0x01u
#define MP2731_REG_IIN_LIMIT 0x00u
#define MP2731_REG_AICO      0x02u   /* REG02 bit 0 AICO_EN; bits 7:1 NTC/TJ_REG */
#define MP2731_AICO_EN_BIT   0x01u
#define MP2731_REG_IIN_DPM   0x14u   /* [5:0] limit AICO applies, 100 mA + 50 mA steps */
#define MP2731_REG_DPM_MASK  0x15u   /* bit 7 AICO_STAT */
#define MP2731_AICO_STAT_BIT 0x80u

#define MP2731_I2C_TIMEOUT_TICKS pdMS_TO_TICKS(1000)

static bool s_ready = false;

/* One power read is a multi-transaction sequence (clear, start, poll, read).
 * Four tasks call it (publish gate, SD guard, heartbeat, CLI); interleaved, one
 * caller's "clear START" would abort another's conversion and surface as a
 * spurious adc_timeout. Serialise whole sequences, not just bus transactions. */
static SemaphoreHandle_t s_read_mtx;
static mp2731_frame_t    s_last_frame;    /* diagnostics (CLI), under s_read_mtx */
static esp_err_t         s_last_err = ESP_ERR_INVALID_STATE;
/* Uptime (ms) at which s_last_frame was captured; -1 = never. The frame itself
 * carries no time, so a reader of the cache (bench BLE `live`) could not tell a
 * 5 s old frame from one an hour old (charger reads stop when their callers
 * do). Same lock as the frame; written only beside it. */
static int64_t           s_last_frame_ms = -1;
static mp2731_history_t s_history;
static portMUX_TYPE s_history_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_overlap_epoch;
static unsigned s_power_reads_active;

/* ── Charger re-detection state (docs/charger-redetect.md) ────────────────
 * Everything below is guarded by s_read_mtx: every power read feeds the
 * policy (mp2731_redetect.c) while it still holds the mutex, the heartbeat
 * render snapshots it under the same mutex, and the switches take it too. */
static crd_t   s_crd;
static bool  (*s_redetect_veto)(void);
static bool    s_prev_adc_valid;
static uint8_t s_prev_adc[5];
/* BG_EN may be left at 1 (a clear failed on the bus): every later read
 * retries the clear first. The bandgap "Disabled" state must not persist. */
static bool    s_bg_en_dirty;
/* Same for REG0B bit 5 USB_DET_EN after a manual dpdm pulse (HAR-310 #1): a
 * restore that could not be PROVEN (bus error, or bit 5 still reads 1) leaves
 * this set. "Force DP/DM detection" left asserted may keep re-running input
 * detection - dropping the input each time - through a later OTA or AMBIT
 * flash that relies on external power. While either flag is set no pulse of
 * any kind is allowed, AICO holds, `auto on` is refused, and every power read
 * retries the restore first (pulse_clear_if_dirty). */
static bool    s_dpdm_dirty;

static bool charger_dirty(void)
{
    return s_bg_en_dirty || s_dpdm_dirty;
}

static void dirty_sync(void)
{
    s_crd.dirty = (uint8_t)((s_bg_en_dirty ? CRD_DIRTY_BG_EN : 0u) |
                            (s_dpdm_dirty ? CRD_DIRTY_DPDM : 0u));
}
static bool    s_manual_active;
static crd_block_t s_logged_block = CRD_BLOCK_NONE;

/* AICO re-arm policy (docs/charger-aico.md), also guarded by s_read_mtx. */
static aico_t  s_aico;

/* Charger mutation admission (mp2731_set_mutation_gate). Set once by the
 * composition root before any read; NULL = always admitted. */
static bool  (*s_gate_begin)(void);
static void  (*s_gate_end)(void);
static bool  (*s_gate_adopt)(void);   /* mp2731_set_quarantine_adopt */

static bool mutation_begin(void)
{
    return s_gate_begin == NULL || s_gate_begin();
}

static void mutation_end(void)
{
    if (s_gate_begin != NULL && s_gate_end != NULL) s_gate_end();
}

/* ── Quarantine of a dirty charger bit (HAR-310, Codex review of PR #112) ──
 * A BG_EN or USB_DET_EN not proven back at 0 is not only a pulse hazard: its
 * RECOVERY is one too. Clearing BG_EN is a 1->0 edge, which by the datasheet
 * (Rev 1.2 p.34) "resets the input plug-in detection and safety timer" - the
 * very re-detect edge, and on the bench it restarted a charge cycle on a full
 * battery. Clearing USB_DET_EN ends a forced DP/DM detection. Neither may
 * happen while an OTA or AMBIT flash runs on the external input.
 *
 * So, while either bit is dirty, this driver HOLDS one maintenance-gate
 * admission (s_quarantine). The gate counts it like an in-flight mutation:
 * a maintenance request becomes pending and waits (APP_CHARGER_DRAIN_MS) or
 * gives up busy; it is not granted over a dirty bit (rebuilt from the
 * registers at init after an ESP-only reboot: charger_reconcile). The
 * admission is the one the failing pulse already held, handed over before it
 * would have been released (quarantine_take_over), so there is no instant
 * with mutations = 0 and the bit dirty. Recovery (pulse_clear_if_dirty, on every power read)
 * needs no new admission - mutation_begin would refuse it while maintenance
 * is pending and strand the bit - and it can never meet GRANTED maintenance,
 * because maintenance cannot be granted while the quarantine is held. Once
 * both bits read back 0 the admission is released and a pending maintenance
 * request is granted on its next poll. */
static bool s_quarantine;   /* under s_read_mtx */

/* Under s_read_mtx, at the end of a pulse that holds an admission. True =
 * the caller must NOT release it: it now belongs to the quarantine. */
static bool quarantine_take_over(void)
{
    /* Without a gate there is nothing to hold (mutation_end is a no-op);
     * mp2731_set_mutation_gate adopts the dirty state when one is wired. */
    if (!charger_dirty() || s_quarantine || s_gate_begin == NULL) return false;
    s_quarantine = true;
    ESP_LOGW(TAG, "charger re-detect: %s%s%s not proven back at 0; charger quarantined, "
                  "maintenance waits until it is",
             s_bg_en_dirty ? "BG_EN" : "", s_bg_en_dirty && s_dpdm_dirty ? " and " : "",
             s_dpdm_dirty ? "USB_DET_EN" : "");
    return true;
}

/* Under s_read_mtx (or single-threaded boot): take a NEW admission for a
 * dirty state that has none - found at boot, or the gate wired after it.
 * Refused (maintenance active) = nothing held; recovery then writes nothing
 * until it can take one (pulse_clear_if_dirty). */
static void quarantine_adopt(void)
{
    if (!charger_dirty() || s_quarantine || s_gate_begin == NULL) return;
    /* Adopt, not begin: a maintenance request already PENDING (made during
     * the boot hold) must not refuse it, or it would be granted over the
     * dirty bit as soon as the hold ends. Only RUNNING maintenance refuses. */
    if (s_gate_adopt != NULL ? s_gate_adopt() : mutation_begin()) {
        s_quarantine = true;
    } else {
        ESP_LOGW(TAG, "charger re-detect: dirty charger bit but maintenance already active; "
                      "recovery held until it ends");
    }
}

/* Under s_read_mtx: release the quarantine once both bits are proven 0. */
static void quarantine_release_if_clean(void)
{
    if (!s_quarantine || charger_dirty()) return;
    s_quarantine = false;
    mutation_end();
    ESP_LOGW(TAG, "charger re-detect: quarantine released, maintenance admitted again");
}

/* Reset guard. A pulse that crashed the unit (an unknown silicon reaction to
 * the bandgap toggle collapsing VSYS, a hung bus tripping a watchdog) must
 * not be repeated after every boot: the marker is open from the pulse until
 * the outcome window closes (auto) or the report is complete (manual). Found
 * open at boot after anything but a power-on / deliberate software restart,
 * automatic re-detection stays off for that boot (RAM only, like the rest).
 * RTC_NOINIT survives CPU, watchdog, panic and brownout resets; a real power
 * loss garbles it, which the magic plus the POWERON check reject. */
#define REDETECT_MARKER_OPEN 0x52445431u   /* "RDT1" */
static RTC_NOINIT_ATTR uint32_t s_redetect_marker;
static bool s_marker_checked;

static void marker_sync(void)
{
    s_redetect_marker = (s_crd.pending || s_manual_active) ? REDETECT_MARKER_OPEN : 0u;
}

static uint64_t history_begin(void)
{
    portENTER_CRITICAL(&s_history_mux);
    if (s_power_reads_active != 0u) s_overlap_epoch++;
    s_power_reads_active++;
    const uint64_t epoch = s_overlap_epoch;
    portEXIT_CRITICAL(&s_history_mux);
    return epoch;
}

static void history_finish(uint64_t epoch, esp_err_t err, const power_reading_t *p,
                            const mp2731_frame_t *f)
{
    portENTER_CRITICAL(&s_history_mux);
    if (epoch != s_overlap_epoch || s_power_reads_active > 1u)
        s_history.previous_status_valid = false;
    s_power_reads_active--;
    /* Timestamp inside the critical section: delayed task completions are
     * ordered here, never allowed to move history backwards. No bus calls. */
    mp2731_history_observe(&s_history, esp_timer_get_time() / 1000, err, p, f);
    portEXIT_CRITICAL(&s_history_mux);
}

static esp_err_t mp2731_read_reg(uint8_t reg, uint8_t *out)
{
    i2c_port_t port = I2C_NUM_MAX;
    esp_err_t err = i2c_bus_get_port(&port);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_bus_lock(MP2731_I2C_TIMEOUT_TICKS);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_master_write_read_device(port, MP2731_I2C_ADDR, &reg, 1, out, 1,
                                       MP2731_I2C_TIMEOUT_TICKS);
    (void)i2c_bus_unlock();

    if (err != ESP_OK && out != NULL) {
        *out = 0;
    }
    return err;
}

static esp_err_t mp2731_write_reg(uint8_t reg, uint8_t val)
{
    i2c_port_t port = I2C_NUM_MAX;
    esp_err_t err = i2c_bus_get_port(&port);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_bus_lock(MP2731_I2C_TIMEOUT_TICKS);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t buf[2] = {reg, val};
    err = i2c_master_write_to_device(port, MP2731_I2C_ADDR, buf, sizeof(buf),
                                     MP2731_I2C_TIMEOUT_TICKS);
    (void)i2c_bus_unlock();
    return err;
}

/* ── Boot reconciliation (HAR-310, Codex re-review of PR #112) ───────────
 * The quarantine, the dirty flags and the maintenance gate all live in ESP
 * RAM; the MP2731 is powered separately and an ESP reset is not a charger
 * POR (its I2C watchdog has long expired, so nothing resets REG09/REG0B
 * either). After any ESP-only reboot the chip can still hold BG_EN = 1 or
 * USB_DET_EN = 1 from a pulse whose cleanup was never proven. The REGISTERS
 * are therefore the source of truth: read both at init and rebuild the dirty
 * flags (and with them the quarantine). A register that cannot be read is
 * treated as set - fail closed; recovery reads it again on every power read
 * and writes only a bit it sees set. POR is 0 for both bits and nothing but
 * a re-detect pulse sets them, so a healthy unit is never quarantined.
 * Not covered: a charger that fails its boot probe is not driven at all this
 * boot (no reads, no writes), and nothing can be reconciled. */
static void charger_reconcile(void)
{
    uint8_t r09 = 0, r0b = 0;
    const esp_err_t e09 = mp2731_read_reg(MP2731_REG_BANDGAP, &r09);
    const esp_err_t e0b = mp2731_read_reg(MP2731_REG_USB_DET, &r0b);
    if (e09 != ESP_OK || (r09 & MP2731_BG_EN_BIT) != 0u) s_bg_en_dirty = true;
    if (e0b != ESP_OK || (r0b & MP2731_USB_DET_EN_BIT) != 0u) s_dpdm_dirty = true;
    dirty_sync();
    if (!charger_dirty()) return;
    ESP_LOGW(TAG, "charger re-detect at boot: REG09 %s0x%02X, REG0B %s0x%02X - %s%s%s not proven 0; "
                  "charger quarantined, maintenance waits until recovery proves it",
             e09 != ESP_OK ? "unreadable " : "", r09, e0b != ESP_OK ? "unreadable " : "", r0b,
             s_bg_en_dirty ? "BG_EN" : "", s_bg_en_dirty && s_dpdm_dirty ? " and " : "",
             s_dpdm_dirty ? "USB_DET_EN" : "");
    quarantine_adopt();
}

esp_err_t mp2731_init(void)
{
    i2c_port_t port = I2C_NUM_MAX;
    esp_err_t err = i2c_bus_get_port(&port);
    if (err != ESP_OK) {
        s_ready = false;
        return err;
    }

    if (s_read_mtx == NULL) {
        s_read_mtx = xSemaphoreCreateMutex();
        if (s_read_mtx == NULL) {
            s_ready = false;
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_marker_checked) {
        s_marker_checked = true;
        crd_init(&s_crd);
        aico_init(&s_aico);
        const esp_reset_reason_t why = esp_reset_reason();
        if (s_redetect_marker == REDETECT_MARKER_OPEN && why != ESP_RST_POWERON &&
            why != ESP_RST_SW && why != ESP_RST_DEEPSLEEP) {
            crd_set_disabled(&s_crd, CRD_DISABLED_RESET);
            ESP_LOGW(TAG, "charger re-detect: unit reset (reason %d) while a re-detect was open; "
                          "automatic re-detection OFF for this boot (re-enable: charger_redetect auto on)",
                     (int)why);
        }
        s_redetect_marker = 0u;
    }
    err = i2c_bus_probe_addr(port, MP2731_I2C_ADDR);
    s_ready = (err == ESP_OK);
    if (s_ready) {
        ESP_LOGI(TAG, "MP2731 charger detected @ 0x%02X", MP2731_I2C_ADDR);
        charger_reconcile();
    } else {
        ESP_LOGW(TAG, "MP2731 not found @ 0x%02X: %s", MP2731_I2C_ADDR,
                 esp_err_to_name(err));
    }
    return err;
}

bool mp2731_is_ready(void)
{
    return s_ready;
}

static esp_err_t bus_read(void *ctx, uint8_t reg, uint8_t *val)
{
    (void)ctx;
    return mp2731_read_reg(reg, val);
}

static esp_err_t bus_write(void *ctx, uint8_t reg, uint8_t val)
{
    (void)ctx;
    return mp2731_write_reg(reg, val);
}

static void bus_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1);
}

static const mp2731_bus_t s_bus = {
    .read = bus_read, .write = bus_write, .delay_ms = bus_delay, .ctx = NULL,
};

/* ── Re-detection: register pulses ───────────────────────────────────────
 * The pulse holds the shared I2C bus for its whole set/verify/clear/verify
 * sequence, so no other bus user (RTC, BME280, a CLI read) can stretch the
 * time BG_EN spends at 1. These accessors run inside that lock. */
static esp_err_t locked_read(void *ctx, uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(*(const i2c_port_t *)ctx, MP2731_I2C_ADDR, &reg, 1,
                                        val, 1, MP2731_I2C_TIMEOUT_TICKS);
}

static esp_err_t locked_write(void *ctx, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(*(const i2c_port_t *)ctx, MP2731_I2C_ADDR, buf,
                                      sizeof(buf), MP2731_I2C_TIMEOUT_TICKS);
}

typedef enum { PULSE_BG_EN, PULSE_DPDM_FORCE, PULSE_DPDM_RESTORE, PULSE_BG_EN_CLEAR } pulse_op_t;

static esp_err_t pulse_locked(pulse_op_t op, mp2731_pulse_t *out)
{
    /* Zeroed before ANY exit (HAR-310 #2): a get-port or bus-lock failure
     * returns before the core runs, and callers read set_attempted / cleared
     * to decide whether a write may have landed. Garbage there marked BG_EN
     * dirty (or not) at random and logged a pulse that never happened. */
    memset(out, 0, sizeof *out);
    i2c_port_t port = I2C_NUM_MAX;
    esp_err_t err = i2c_bus_get_port(&port);
    if (err != ESP_OK) return err;
    err = i2c_bus_lock(MP2731_I2C_TIMEOUT_TICKS);
    if (err != ESP_OK) return err;
    const mp2731_bus_t bus = {.read = locked_read, .write = locked_write,
                              .delay_ms = bus_delay, .ctx = &port};
    bool wrote = false;
    switch (op) {
    case PULSE_BG_EN:
        err = mp2731_core_bg_en_pulse(&bus, out);
        break;
    case PULSE_DPDM_FORCE:
        err = mp2731_core_dpdm_force(&bus, out);
        break;
    case PULSE_DPDM_RESTORE:
        err = mp2731_core_dpdm_restore(&bus, &out->after, &wrote);
        out->clear_writes = wrote ? 1u : 0u;
        out->cleared = err == ESP_OK;
        break;
    case PULSE_BG_EN_CLEAR: {
        uint8_t v = 0;
        err = locked_read(&port, MP2731_REG_BANDGAP, &v);
        if (err == ESP_OK && (v & MP2731_BG_EN_BIT) != 0u) {
            err = locked_write(&port, MP2731_REG_BANDGAP, (uint8_t)(v & (uint8_t)~MP2731_BG_EN_BIT));
            if (err == ESP_OK) err = locked_read(&port, MP2731_REG_BANDGAP, &v);
        }
        out->after = v;
        out->cleared = err == ESP_OK && (v & MP2731_BG_EN_BIT) == 0u;
        break;
    }
    }
    (void)i2c_bus_unlock();
    return err;
}

/* One REG00..REG17 scan for a manual report; stops on the first bus error. */
static void snapshot(power_charger_snapshot_t *snap, int64_t t_ms)
{
    snap->t_ms = (int32_t)t_ms;
    snap->mask = 0;
    memset(snap->regs, 0, sizeof snap->regs);
    for (uint8_t reg = 0; reg < POWER_CHARGER_REG_COUNT; reg++) {
        if (mp2731_read_reg(reg, &snap->regs[reg]) != ESP_OK) break;
        snap->mask |= 1u << reg;
    }
}

static int64_t uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Under s_read_mtx. A BG_EN or USB_DET_EN that might still be 1 is cleared
 * before anything else happens on the charger.
 *
 * NOT harmless: a BG_EN 1->0 is the re-detect edge itself (input detection
 * and safety timer reset, a charge cycle can restart), and clearing
 * USB_DET_EN ends a forced detection. It is exempt from mutation_begin only
 * in one sense: it runs under the quarantine admission the failing pulse
 * handed over, so it may run while maintenance is PENDING (and must, or the
 * bit would stay dirty behind the waiting request), and it can never run
 * while maintenance is GRANTED (the held admission prevents the grant). */
static void pulse_clear_if_dirty(void)
{
    if (!charger_dirty()) return;
    if (!s_quarantine && s_gate_begin != NULL) {
        /* Only while a manual bg_en that dirtied BG_EN is still in its settle
         * reads: its own admission is held (maintenance cannot be granted),
         * but it is not the quarantine yet. Take a second admission for the
         * recovery, or write nothing: a refusal means maintenance is pending,
         * and the manual command hands its admission over when it ends. */
        if (!mutation_begin()) return;
        s_quarantine = true;
    }
    mp2731_pulse_t p;
    if (s_bg_en_dirty && pulse_locked(PULSE_BG_EN_CLEAR, &p) == ESP_OK && p.cleared) {
        s_bg_en_dirty = false;
        ESP_LOGW(TAG, "charger re-detect: BG_EN cleared (REG09 0x%02X)", p.after);
    }
    if (s_dpdm_dirty && pulse_locked(PULSE_DPDM_RESTORE, &p) == ESP_OK && p.cleared) {
        s_dpdm_dirty = false;
        ESP_LOGW(TAG, "charger re-detect: USB_DET_EN restored to 0 (REG0B 0x%02X)", p.after);
    }
    dirty_sync();
    quarantine_release_if_clean();
}

/* Under s_read_mtx: the dwell is met. Precheck, pulse, record, log once. */
static void redetect_fire(int64_t now, const crd_obs_t *o)
{
    uint8_t r00 = 0, r01 = 0, r04 = 0, r17 = 0;
    crd_block_t block = CRD_BLOCK_NONE;
    if (mp2731_read_reg(0x00, &r00) != ESP_OK || mp2731_read_reg(0x01, &r01) != ESP_OK ||
        mp2731_read_reg(0x04, &r04) != ESP_OK || mp2731_read_reg(0x17, &r17) != ESP_OK) {
        block = CRD_BLOCK_BUS;
    } else {
        block = crd_precheck(r00, r01, r04, r17, o->vin_mv);
    }
    if (block != CRD_BLOCK_NONE) {
        crd_note_blocked(&s_crd, now, block);
        if (block != s_logged_block) {   /* once per reason, not every recheck */
            ESP_LOGW(TAG, "charger re-detect held: %s (VIN %u mV, VSYS %u mV, REG0C 0x%02X, "
                          "REG00 0x%02X REG01 0x%02X REG04 0x%02X REG17 0x%02X)",
                     crd_block_name(block), o->vin_mv, o->vsys_mv, o->status, r00, r01, r04, r17);
            s_logged_block = block;
        }
        return;
    }
    s_logged_block = CRD_BLOCK_NONE;

    /* The veto probe in crd_step is a snapshot: maintenance may have begun
     * since. The admission is the exclusion; refused = held like a veto (the
     * next read sees maintenance active and holds through crd_step). */
    if (!mutation_begin()) {
        if (!s_crd.veto_counted) s_crd.vetoed++;
        s_crd.veto_counted = true;
        return;
    }
    mp2731_pulse_t p = {0};
    s_redetect_marker = REDETECT_MARKER_OPEN;   /* before the write, see marker_sync */
    const esp_err_t err = pulse_locked(PULSE_BG_EN, &p);
    crd_pulse_t result = CRD_PULSE_OK;
    if (err == ESP_ERR_INVALID_RESPONSE) {
        result = CRD_PULSE_NOT_LATCHED;
    } else if (err == ESP_ERR_INVALID_STATE) {
        result = CRD_PULSE_NOT_CLEARED;
        s_bg_en_dirty = true;
    } else if (err != ESP_OK) {
        result = CRD_PULSE_BUS_ERROR;
        /* The set write may have landed and no clear was verified. */
        if (p.set_attempted && !p.cleared) s_bg_en_dirty = true;
    }
    dirty_sync();
    /* A dirty BG_EN keeps this pulse's admission as the quarantine: released
     * only after the dirty flag is visible, never before. */
    if (!quarantine_take_over()) mutation_end();
    const int64_t dwell_s = (now - s_crd.dwell_start_ms) / 1000;
    (void)dwell_s;   /* log-only */
    crd_note_attempt(&s_crd, now, result, o->status, o->vin_mv, o->vsys_mv);
    marker_sync();
    if (!p.set_attempted) {
        /* The bus was never ours (get-port / lock failure): nothing was
         * written, so this is logged as a failed attempt, never as a pulse. */
        ESP_LOGW(TAG, "charger re-detect #%u: BG_EN pulse not started, I2C bus unavailable (%s); "
                      "nothing written, next in >= %us", (unsigned)s_crd.attempts,
                 esp_err_to_name(err), (unsigned)(s_crd.backoff_ms / 1000u));
        return;
    }
    ESP_LOGW(TAG, "charger re-detect #%u: no input and not charging for %llds with VIN %u mV, "
                  "VSYS %u mV (REG0C 0x%02X REG0D 0x%02X): BG_EN pulse REG09 0x%02X->0x%02X->0x%02X %s",
             (unsigned)s_crd.attempts, (long long)dwell_s, o->vin_mv, o->vsys_mv, o->status,
             o->fault, p.before, p.set_rb, p.after,
             result == CRD_PULSE_OK ? "ok" : crd_outcome_name(s_crd.last_outcome));
    if (result == CRD_PULSE_NOT_CLEARED) {
        ESP_LOGE(TAG, "charger re-detect: BG_EN still 1 after %u clears; automatic re-detection "
                      "disabled, clear retried on every read", (unsigned)p.clear_writes);
    }
}

/* Under s_read_mtx, after every power read (any verdict). */
static void redetect_after_read(esp_err_t err, const power_reading_t *p, const mp2731_frame_t *f)
{
    pulse_clear_if_dirty();
    const int64_t now = uptime_ms();
    crd_obs_t o = {.now_ms = now, .status_valid = f->status_valid, .adc_ok = err == ESP_OK,
                   .status = f->status, .fault = f->fault};
    if (o.adc_ok) {
        const uint8_t adc[5] = {f->vbat, f->vsys, f->vin, f->icc, f->iin};
        o.adc_changed = s_prev_adc_valid && memcmp(adc, s_prev_adc, sizeof adc) != 0;
        memcpy(s_prev_adc, adc, sizeof adc);
        s_prev_adc_valid = true;
        o.vin_mv = p->input_mv;
        o.vsys_mv = p->system_mv;
    }
    /* Never pulse again over a BG_EN or USB_DET_EN not yet proven back at 0. */
    const bool veto = charger_dirty() || (s_redetect_veto != NULL && s_redetect_veto());
    const crd_action_t action = crd_step(&s_crd, &o, veto);
    if (action == CRD_RESOLVED) {
        marker_sync();
        ESP_LOGI(TAG, "charger re-detect #%u outcome: %s (REG0C 0x%02X -> 0x%02X), next in >= %us",
                 (unsigned)s_crd.attempts, crd_outcome_name(s_crd.last_outcome), s_crd.last_before,
                 s_crd.last_after, (unsigned)(s_crd.backoff_ms / 1000u));
    } else if (action == CRD_CHECK) {
        redetect_fire(now, &o);
    }
}

/* ── AICO: register access ──────────────────────────────────────────────
 * Every AICO access is ONE bus hold, like the re-detect pulse: no other bus
 * user can interleave a REG00/REG02 change between the read and the write,
 * and the reset can never land inside a BG_EN pulse (which holds the bus for
 * its whole set/verify/clear/verify sequence). */
typedef enum {
    AICO_OP_READ,           /* REG02, REG00, REG14, REG15 */
    AICO_OP_RESET_IF_ON,    /* automatic: rewrite REG00 only while AICO_EN reads 1 */
    AICO_OP_RESET,          /* manual: rewrite REG00 now */
    AICO_OP_SET_ON,
    AICO_OP_SET_OFF,
} aico_op_t;

typedef struct {
    uint8_t   r02, r00, r14_before, r14, r15;
    bool      wrote;        /* the mutation's write was issued */
    bool      skipped_off;  /* RESET_IF_ON read AICO_EN = 0: nothing else read or written */
    bool      status_ok;    /* every read AFTER the verified mutation succeeded */
    esp_err_t status_err;
} aico_io_t;

/* Returns the outcome of the decisive part only: for READ every read; for a
 * mutation the reads it depends on, the write and its readback (ESP_OK =
 * written and verified). Reads AFTER a verified mutation are observation:
 * their failure is reported in status_ok and never turns a verified mutation
 * into a failed one, which the caller would otherwise retry and replay
 * (Codex review of PR #109: three verified rewrites in 11 min, resets=0). */
static esp_err_t aico_io(aico_op_t op, aico_io_t *io)
{
    memset(io, 0, sizeof *io);
    io->status_ok = true;
    const bool set = op == AICO_OP_SET_ON || op == AICO_OP_SET_OFF;
    const bool reset = op == AICO_OP_RESET || op == AICO_OP_RESET_IF_ON;
    i2c_port_t port = I2C_NUM_MAX;
    esp_err_t err = i2c_bus_get_port(&port);
    if (err != ESP_OK) return err;
    err = i2c_bus_lock(MP2731_I2C_TIMEOUT_TICKS);
    if (err != ESP_OK) return err;
    err = locked_read(&port, MP2731_REG_AICO, &io->r02);
    if (err == ESP_OK && op == AICO_OP_RESET_IF_ON && (io->r02 & MP2731_AICO_EN_BIT) == 0u) {
        io->skipped_off = true;          /* the off-state look is this one read */
        (void)i2c_bus_unlock();
        return ESP_OK;
    }
    if (err == ESP_OK && set) {
        /* Only bit 0. Bits 7:1 (shipping delay, NTC type/enables, TJ_REG,
         * NTC option) are copied from the read: unlike AICO_EN they ARE reset
         * by the expired watchdog, so a stale cached value must never be
         * written over them. */
        const uint8_t want = (uint8_t)((io->r02 & (uint8_t)~MP2731_AICO_EN_BIT) |
                                       (op == AICO_OP_SET_ON ? MP2731_AICO_EN_BIT : 0u));
        io->wrote = true;
        err = locked_write(&port, MP2731_REG_AICO, want);
        if (err == ESP_OK) err = locked_read(&port, MP2731_REG_AICO, &io->r02);
        if (err == ESP_OK && io->r02 != want) err = ESP_ERR_INVALID_RESPONSE;
    } else if (err == ESP_OK && reset) {
        /* "Any write to IIN_LIM can reset IIN_DPM to the same IIN_LIM value
         * when the AICO function is enabled" (Rev 1.2 p.21). The byte goes
         * back exactly as read: HIZ and EN_ILIM (bits 7:6) and the register
         * limit are unchanged; only AICO's learned limit is released. */
        err = locked_read(&port, MP2731_REG_IIN_LIMIT, &io->r00);
        if (err == ESP_OK) err = locked_read(&port, MP2731_REG_IIN_DPM, &io->r14_before);
        if (err == ESP_OK) {
            uint8_t rb = 0;
            io->wrote = true;
            err = locked_write(&port, MP2731_REG_IIN_LIMIT, io->r00);
            if (err == ESP_OK) err = locked_read(&port, MP2731_REG_IIN_LIMIT, &rb);
            if (err == ESP_OK && rb != io->r00) err = ESP_ERR_INVALID_RESPONSE;
        }
    } else if (err == ESP_OK) {          /* AICO_OP_READ */
        err = locked_read(&port, MP2731_REG_IIN_LIMIT, &io->r00);
        if (err == ESP_OK) err = locked_read(&port, MP2731_REG_IIN_DPM, &io->r14_before);
        if (err == ESP_OK) err = locked_read(&port, MP2731_REG_DPM_MASK, &io->r15);
        io->r14 = io->r14_before;
    }
    if (err == ESP_OK && (set || reset)) {
        esp_err_t s = ESP_OK;
        if (set) s = locked_read(&port, MP2731_REG_IIN_LIMIT, &io->r00);
        if (s == ESP_OK) s = locked_read(&port, MP2731_REG_IIN_DPM, &io->r14);
        if (set) io->r14_before = io->r14;
        if (s == ESP_OK) s = locked_read(&port, MP2731_REG_DPM_MASK, &io->r15);
        io->status_ok = s == ESP_OK;
        io->status_err = s;
    }
    (void)i2c_bus_unlock();
    return err;
}

/* Under s_read_mtx: anything that must not see a REG00 rewrite right now.
 * The maintenance lock (OTA / AMBIT flash run on external power: no input
 * disturbance), a manual re-detect in progress (its settle snapshots must see
 * only the pulse), an automatic re-detect whose outcome is being judged, and
 * a BG_EN or USB_DET_EN not yet proven back at 0. This is the policy's HOLD;
 * the exclusion against maintenance itself is the admission taken around the
 * I/O. */
static bool aico_vetoed(void)
{
    return s_manual_active || s_crd.pending || charger_dirty() ||
           (s_redetect_veto != NULL && s_redetect_veto());
}

static uint16_t iin_limit_ma(uint8_t reg00);

static uint16_t iin_dpm_ma(uint8_t reg14)
{
    return (uint16_t)(POWER_IIN_LIMIT_MIN_MA + (reg14 & POWER_IIN_LIMIT_REG_MASK) * POWER_IIN_LIMIT_STEP_MA);
}

/* Under s_read_mtx, after every power read, after the re-detect step (so a
 * pulse fired by this very read already shows as pending and vetoes). */
static void aico_after_read(void)
{
    const int64_t now = uptime_ms();
    if (aico_step(&s_aico, now, aico_vetoed()) != AICO_CHECK) return;
    /* Admission for the whole bus sequence: maintenance cannot begin between
     * the REG02 decision and the REG00 write. Refused = held like a veto. */
    if (!mutation_begin()) {
        aico_note_refused(&s_aico);
        return;
    }
    aico_io_t io;
    const esp_err_t err = aico_io(AICO_OP_RESET_IF_ON, &io);
    mutation_end();
    if (err != ESP_OK) {
        aico_note_result(&s_aico, now, AICO_RESULT_FAILED);
        ESP_LOGW(TAG, "AICO re-arm failed (%s%s); retry in %us", esp_err_to_name(err),
                 io.wrote ? ", REG00 write issued but not verified" : ", nothing written",
                 (unsigned)(AICO_RETRY_MS / 1000));
        return;
    }
    if (io.skipped_off) {
        aico_note_result(&s_aico, now, AICO_RESULT_OFF);
        return;
    }
    aico_note_result(&s_aico, now, AICO_RESULT_RESET);
    if (io.status_ok) {
        ESP_LOGI(TAG, "AICO re-armed #%u: REG00 0x%02X rewritten, IIN_DPM %u -> %u mA (REG15 0x%02X)",
                 (unsigned)s_aico.resets, io.r00, iin_dpm_ma(io.r14_before), iin_dpm_ma(io.r14), io.r15);
    } else {
        ESP_LOGW(TAG, "AICO re-armed #%u: REG00 0x%02X rewritten and verified; status read after it "
                      "failed (%s), not retried", (unsigned)s_aico.resets, io.r00,
                 esp_err_to_name(io.status_err));
    }
}

/* Under s_read_mtx. Registers that were not read (a status read after a
 * verified mutation failed) are reported as such, never as zero. */
static void aico_fill(power_aico_t *out, const aico_io_t *io, int64_t now)
{
    *out = (power_aico_t){
        .enabled = (io->r02 & MP2731_AICO_EN_BIT) != 0u,
        .status_valid = io->status_ok,
        .aico_stat = io->status_ok && (io->r15 & MP2731_AICO_STAT_BIT) != 0u,
        .reg02 = io->r02, .reg00 = io->r00, .reg14 = io->r14, .reg15 = io->r15,
        .iin_limit_ma = iin_limit_ma(io->r00), .iin_dpm_ma = iin_dpm_ma(io->r14),
        .resets = s_aico.resets, .failed = s_aico.failed,
        .has_reset = s_aico.has_reset, .last_reset_ms = s_aico.last_reset_ms,
        .next_check_in_s = aico_next_in_s(&s_aico, now),
    };
}

esp_err_t mp2731_get_vin_limit(power_vin_limit_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t reg = 0;
    esp_err_t err = mp2731_read_reg(MP2731_REG_VIN_LIMIT, &reg);
    if (err != ESP_OK) return err;
    const uint16_t mv = POWER_VIN_LIMIT_MIN_MV + (reg & 0x7FU) * POWER_VIN_LIMIT_STEP_MV;
    if ((reg & 0x80U) != 0 || mv > POWER_VIN_LIMIT_MAX_MV) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = (power_vin_limit_t){.mv = mv, .reg01 = reg};
    return ESP_OK;
}

esp_err_t mp2731_set_vin_limit(uint16_t mv, power_vin_limit_t *out)
{
    if (out == NULL || !power_vin_limit_valid_mv(mv)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    /* Held across read/write/readback: maintenance cannot begin mid-set. */
    if (!mutation_begin()) return ESP_ERR_NOT_ALLOWED;

    i2c_port_t port = I2C_NUM_MAX;
    esp_err_t err = i2c_bus_get_port(&port);
    if (err == ESP_OK) err = i2c_bus_lock(MP2731_I2C_TIMEOUT_TICKS);
    if (err != ESP_OK) {
        mutation_end();
        return err;
    }

    /* REG01[6:0] = (VIN_MIN - 3.7 V) / 100 mV. Bit 7 is REGISTER RESET:
     * deliberately write it as zero, rather than copying a transient reset
     * bit from a read-modify-write. Hold the bus through readback so a CLI and
     * MQTT set cannot interleave and acknowledge each other's values. */
    const uint8_t value = (mv - POWER_VIN_LIMIT_MIN_MV) / POWER_VIN_LIMIT_STEP_MV;
    const uint8_t buf[2] = {MP2731_REG_VIN_LIMIT, value};
    err = i2c_master_write_to_device(port, MP2731_I2C_ADDR, buf, sizeof(buf),
                                    MP2731_I2C_TIMEOUT_TICKS);
    uint8_t readback = 0;
    if (err == ESP_OK) {
        const uint8_t reg = MP2731_REG_VIN_LIMIT;
        err = i2c_master_write_read_device(port, MP2731_I2C_ADDR, &reg, 1,
                                          &readback, 1, MP2731_I2C_TIMEOUT_TICKS);
        if (err == ESP_OK && readback != value) err = ESP_ERR_INVALID_RESPONSE;
    }
    (void)i2c_bus_unlock();
    if (err == ESP_OK) *out = (power_vin_limit_t){.mv = mv, .reg01 = readback};
    mutation_end();
    return err;
}

static uint16_t iin_limit_ma(uint8_t reg00)
{
    return (uint16_t)(POWER_IIN_LIMIT_MIN_MA + (reg00 & POWER_IIN_LIMIT_REG_MASK) * POWER_IIN_LIMIT_STEP_MA);
}

esp_err_t mp2731_get_iin_limit(power_iin_limit_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t reg = 0;
    esp_err_t err = mp2731_read_reg(MP2731_REG_IIN_LIMIT, &reg);
    if (err != ESP_OK) return err;
    /* All 64 codes decode inside 100..3250 mA, and bits 7:6 (HIZ, EN_ILIM)
     * are legitimate state rather than a corrupt value: report, never refuse. */
    *out = (power_iin_limit_t){.ma = iin_limit_ma(reg), .reg00 = reg};
    return ESP_OK;
}

esp_err_t mp2731_set_iin_limit(uint16_t ma, power_iin_limit_t *out)
{
    if (out == NULL || !power_iin_limit_valid_ma(ma)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    /* Held across read/write/readback: maintenance cannot begin mid-set. */
    if (!mutation_begin()) return ESP_ERR_NOT_ALLOWED;

    i2c_port_t port = I2C_NUM_MAX;
    esp_err_t err = i2c_bus_get_port(&port);
    if (err == ESP_OK) err = i2c_bus_lock(MP2731_I2C_TIMEOUT_TICKS);
    if (err != ESP_OK) {
        mutation_end();
        return err;
    }

    /* REG00 = EN_HIZ(7) | EN_ILIM(6) | IIN_LIM[5:0], IIN_LIM = 100 mA + 50 mA
     * steps (MP2731 Rev 1.2 p.30). Unlike REG01 there is no reset bit to keep
     * clear: bits 7:6 are live controls this command must never change, so the
     * write is a read-modify-write that copies them from the read. HIZ=1 would
     * switch the converter off; clearing EN_ILIM would drop the ILIM-pin cap,
     * the board's hardware safety limit. One bus hold spans read, write and
     * readback so no other bus user can interleave a REG00 change, and a CLI
     * and an MQTT set cannot acknowledge each other's values. A readback that
     * differs in ANY bit (a watchdog reset of 7:6 in the window included) is
     * reported as a failure, never as success. */
    const uint8_t reg = MP2731_REG_IIN_LIMIT;
    uint8_t before = 0;
    uint8_t value = 0;
    uint8_t readback = 0;
    err = i2c_master_write_read_device(port, MP2731_I2C_ADDR, &reg, 1, &before, 1,
                                       MP2731_I2C_TIMEOUT_TICKS);
    if (err == ESP_OK) {
        value = (uint8_t)((before & (uint8_t)~POWER_IIN_LIMIT_REG_MASK) |
                          ((ma - POWER_IIN_LIMIT_MIN_MA) / POWER_IIN_LIMIT_STEP_MA));
        const uint8_t buf[2] = {MP2731_REG_IIN_LIMIT, value};
        err = i2c_master_write_to_device(port, MP2731_I2C_ADDR, buf, sizeof(buf),
                                         MP2731_I2C_TIMEOUT_TICKS);
    }
    if (err == ESP_OK) {
        err = i2c_master_write_read_device(port, MP2731_I2C_ADDR, &reg, 1, &readback, 1,
                                           MP2731_I2C_TIMEOUT_TICKS);
        if (err == ESP_OK && readback != value) err = ESP_ERR_INVALID_RESPONSE;
    }
    (void)i2c_bus_unlock();
    if (err == ESP_OK) *out = (power_iin_limit_t){.ma = ma, .reg00 = readback};
    mutation_end();
    return err;
}

esp_err_t mp2731_read_power(power_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || s_read_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t epoch = history_begin();
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) {
        history_finish(epoch, ESP_ERR_TIMEOUT, NULL, NULL);
        return ESP_ERR_TIMEOUT;
    }
    mp2731_frame_t f;
    const esp_err_t err = mp2731_core_read(&s_bus, out, &f);
    history_finish(epoch, err, out, &f);
    s_last_frame = f;
    s_last_err = err;
    s_last_frame_ms = uptime_ms();
    /* Re-detection rides on the reads that already happen (5-15 s): no task,
     * no polling of its own. The read's verdict and caller see nothing of it. */
    redetect_after_read(err, out, &f);
    /* AICO re-arm rides on the same reads: at most one REG02 read per
     * interval, plus a REG00 rewrite while AICO is on (mp2731_aico.h). */
    aico_after_read();
    xSemaphoreGive(s_read_mtx);
    return err;
}

esp_err_t mp2731_last_frame(mp2731_frame_t *out)
{
    return mp2731_last_frame_at(out, NULL);
}

esp_err_t mp2731_last_frame_at(mp2731_frame_t *out, int64_t *captured_ms)
{
    if (out == NULL || s_read_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_last_frame;
    const esp_err_t err = s_last_err;
    if (captured_ms != NULL) *captured_ms = s_last_frame_ms;
    xSemaphoreGive(s_read_mtx);
    return err;
}

esp_err_t mp2731_probe_conversion(uint8_t *first_ctrl, uint32_t *done_us)
{
    if (first_ctrl == NULL || done_us == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || s_read_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t ctrl = 0;
    esp_err_t err = mp2731_read_reg(MP2731_REG_ADC_CTRL, &ctrl);
    const uint8_t base = (uint8_t)(ctrl & (uint8_t)~(MP2731_ADC_START_BIT | MP2731_ADC_RATE_BIT));
    if (err == ESP_OK && ctrl != base) err = mp2731_write_reg(MP2731_REG_ADC_CTRL, base);
    if (err == ESP_OK) err = mp2731_write_reg(MP2731_REG_ADC_CTRL, base | MP2731_ADC_START_BIT);
    const int64_t t0 = esp_timer_get_time();
    *done_us = UINT32_MAX;
    bool first = true;
    while (err == ESP_OK && esp_timer_get_time() - t0 < (int64_t)MP2731_ADC_TIMEOUT_MS * 1000) {
        uint8_t now = 0;
        err = mp2731_read_reg(MP2731_REG_ADC_CTRL, &now);
        if (err != ESP_OK) break;
        if (first) { *first_ctrl = now; first = false; }
        if ((now & MP2731_ADC_START_BIT) == 0u) {
            *done_us = (uint32_t)(esp_timer_get_time() - t0);
            break;
        }
        esp_rom_delay_us(100);
    }
    if (err == ESP_OK && *done_us == UINT32_MAX) {
        (void)mp2731_write_reg(MP2731_REG_ADC_CTRL, base);
    }
    xSemaphoreGive(s_read_mtx);
    return err;
}

esp_err_t mp2731_read_raw(uint8_t reg, uint8_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return mp2731_read_reg(reg, out);
}

power_read_fn mp2731_get_power_read_fn(void)
{
    return mp2731_read_power;
}

/* Insert `key` + frag before the closing brace of the object in out[0..n).
 * Returns the new length, or n unchanged when it does not fit. */
static int append_member(char *out, size_t cap, int n, const char *key, const char *frag, int m)
{
    const size_t k = strlen(key);
    if (n <= 0 || m <= 0 || out[n - 1] != '}' || (size_t)n - 1u + k + (size_t)m + 1u >= cap) return n;
    n--;
    memcpy(out + n, key, k);
    n += (int)k;
    memcpy(out + n, frag, (size_t)m);
    n += m;
    out[n++] = '}';
    out[n] = '\0';
    return n;
}

/* Slow register scans belong to telemetry, never the 5 s power-control path.
 * Keep the same read mutex as ADC conversions so scans cannot interleave with
 * a conversion sequence. A partial scan is still useful evidence. */
int mp2731_render_diagnostics(char *out, size_t cap, uint64_t *window_id, bool seal)
{
    if (!s_ready || s_read_mtx == NULL || window_id == NULL) return -1;
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) return -1;
    charger_diagnostics_t d;
    const int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_history_mux);
    if (seal) mp2731_history_snapshot(&s_history, &d, now);
    else mp2731_history_peek(&s_history, &d, now);
    portEXIT_CRITICAL(&s_history_mux);
    d.scan_start_ms = now;
    (void)mp2731_diagnostics_read(&s_bus, &d);
    d.scan_end_ms = esp_timer_get_time() / 1000;
    *window_id = d.window_id;
    const crd_t crd = s_crd;
    const aico_t aico = s_aico;
    xSemaphoreGive(s_read_mtx);
    int n = mp2731_diagnostics_render(&d, out, cap);
    /* Append "redetect", then "aico", inside the charger object. Optional:
     * one that does not fit is dropped whole (never truncated), and the
     * charger evidence is reported without it rather than not at all. */
    char frag[CRD_RENDER_CAP];
    int m = crd_render(&crd, now, frag, sizeof frag);
    n = append_member(out, cap, n, ",\"redetect\":", frag, m);
    char aico_frag[AICO_RENDER_CAP];
    m = aico_render(&aico, d.registers, d.read_mask, aico_frag, sizeof aico_frag);
    n = append_member(out, cap, n, ",\"aico\":", aico_frag, m);
    return n;
}

void mp2731_ack_diagnostics(uint64_t window_id)
{
    if (s_read_mtx == NULL) return;
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) return;
    portENTER_CRITICAL(&s_history_mux);
    mp2731_history_ack(&s_history, window_id);
    portEXIT_CRITICAL(&s_history_mux);
    xSemaphoreGive(s_read_mtx);
}

void mp2731_redetect_set_veto(bool (*veto)(void))
{
    s_redetect_veto = veto;
}

void mp2731_set_quarantine_adopt(bool (*adopt)(void))
{
    s_gate_adopt = adopt;
}

void mp2731_set_mutation_gate(bool (*begin)(void), void (*end)(void))
{
    s_gate_begin = begin;
    s_gate_end = end;
    /* A dirty state reconciled at init before the gate was wired becomes a
     * held admission now, so it is visible before any maintenance runs. */
    if (s_read_mtx != NULL && xSemaphoreTake(s_read_mtx, portMAX_DELAY) == pdTRUE) {
        quarantine_adopt();
        xSemaphoreGive(s_read_mtx);
    }
}

static esp_err_t charger_redetect_admitted(power_redetect_method_t method, power_redetect_report_t *out,
                                           bool *keep_admission);

esp_err_t mp2731_charger_redetect(power_redetect_method_t method, power_redetect_report_t *out)
{
    if (out == NULL || (method != POWER_REDETECT_BG_EN && method != POWER_REDETECT_DPDM)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || s_read_mtx == NULL) return ESP_ERR_INVALID_STATE;
    /* OTA / AMBIT flash: a flash runs only on external power, and a pulse on
     * a charger that IS using its input drops the system to battery for the
     * re-detection. Never under the maintenance lock: the admission is held
     * from before the first write to after the DP/DM restore, so maintenance
     * cannot begin in between (it waits for this to end). */
    if (s_redetect_veto != NULL && s_redetect_veto()) return ESP_ERR_NOT_ALLOWED;
    if (!mutation_begin()) return ESP_ERR_NOT_ALLOWED;
    bool keep = false;   /* handed over to the quarantine: see quarantine_take_over */
    const esp_err_t err = charger_redetect_admitted(method, out, &keep);
    if (!keep) mutation_end();
    return err;
}

static esp_err_t charger_redetect_admitted(power_redetect_method_t method, power_redetect_report_t *out,
                                           bool *keep_admission)
{
    *keep_admission = false;
    memset(out, 0, sizeof *out);
    out->method = method;
    out->reg = method == POWER_REDETECT_BG_EN ? MP2731_REG_BANDGAP : MP2731_REG_USB_DET;

    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_manual_active) {             /* CLI and MQTT at once: one at a time */
        xSemaphoreGive(s_read_mtx);
        return ESP_ERR_TIMEOUT;
    }
    /* A previous pulse left a bit not proven back at 0: retry its restore and
     * refuse a new pulse until it is (fail closed, HAR-310 #1). */
    pulse_clear_if_dirty();
    if (charger_dirty()) {
        xSemaphoreGive(s_read_mtx);
        return ESP_ERR_NOT_FINISHED;
    }
    s_manual_active = true;
    s_redetect_marker = REDETECT_MARKER_OPEN;
    const int64_t before = uptime_ms();
    snapshot(&out->snap[0], 0);
    mp2731_pulse_t p = {0};
    const esp_err_t err = pulse_locked(method == POWER_REDETECT_BG_EN ? PULSE_BG_EN : PULSE_DPDM_FORCE, &p);
    const int64_t t0 = uptime_ms();
    out->snap[0].t_ms = (int32_t)(before - t0);
    snapshot(&out->snap[1], uptime_ms() - t0);
    out->snapshots = 2;
    if (method == POWER_REDETECT_BG_EN && err != ESP_OK && err != ESP_ERR_INVALID_RESPONSE &&
        p.set_attempted && !p.cleared) {
        s_bg_en_dirty = true;
        dirty_sync();
    }
    crd_note_manual(&s_crd);
    xSemaphoreGive(s_read_mtx);
    out->reg_before = p.before;
    out->reg_set = p.set_rb;
    out->reg_after = p.after;
    out->latched = p.latched;
    out->cleared = p.cleared;
    if (method == POWER_REDETECT_BG_EN) {
        ESP_LOGW(TAG, "charger re-detect (manual bg_en): REG09 0x%02X->0x%02X->0x%02X REG0C 0x%02X: %s",
                 p.before, p.set_rb, p.after, out->snap[0].regs[0x0C], esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "charger re-detect (manual dpdm): REG0B 0x%02X->0x%02X REG0C 0x%02X: %s",
                 p.before, p.set_rb, out->snap[0].regs[0x0C], esp_err_to_name(err));
    }

    /* Settle snapshots are OPTIONAL evidence: only after a write that reached
     * the chip, and a mutex timeout just skips one. They never carry cleanup. */
    esp_err_t result = err;
    const bool settle = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    const int32_t points[2] = {POWER_REDETECT_SETTLE1_MS, POWER_REDETECT_SETTLE2_MS};
    for (unsigned i = 0; settle && i < 2u; i++) {
        const int64_t wait = t0 + points[i] - uptime_ms();
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS((uint32_t)wait) > 0 ? pdMS_TO_TICKS((uint32_t)wait) : 1);
        if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) continue;
        snapshot(&out->snap[out->snapshots], uptime_ms() - t0);
        out->snapshots++;
        xSemaphoreGive(s_read_mtx);
    }

    /* Cleanup is UNCONDITIONAL (HAR-310 #1). Until v2.23.0 the DP/DM restore
     * lived inside the +5 s snapshot step, so a timed-out mutex take skipped
     * it: the command reported ESP_OK, the admission was released and
     * maintenance could be granted with REG0B bit 5 still forcing detection.
     * Now, whenever a USB_DET_EN write may have landed (also when it read back
     * 0, or the force failed after issuing it), the restore runs here, still
     * inside the admitted mutation, and must PROVE bit 5 = 0. Unproven = the
     * command fails and s_dpdm_dirty keeps every pulse refused while every
     * power read retries the restore. Success is never reported without it. */
    const bool restore = method == POWER_REDETECT_DPDM && p.set_attempted;
    if (xSemaphoreTake(s_read_mtx, portMAX_DELAY) == pdTRUE) {
        if (restore) {
            mp2731_pulse_t r = {0};
            const esp_err_t rerr = pulse_locked(PULSE_DPDM_RESTORE, &r);
            out->restored = r.clear_writes != 0u;
            out->cleared = rerr == ESP_OK && r.cleared;
            if (rerr == ESP_OK || rerr == ESP_ERR_INVALID_STATE) out->reg_after = r.after;
            if (!out->cleared) {
                s_dpdm_dirty = true;
                if (result == ESP_OK) result = rerr != ESP_OK ? rerr : ESP_ERR_INVALID_STATE;
                ESP_LOGE(TAG, "charger re-detect (manual dpdm): USB_DET_EN restore not proven (%s, "
                              "REG0B 0x%02X); pulses refused, restore retried on every read",
                         esp_err_to_name(rerr), r.after);
            }
            dirty_sync();
        }
        /* Either bit dirty (BG_EN from the pulse above, USB_DET_EN from this
         * restore): the command's admission becomes the quarantine, decided
         * under the same mutex that made the flag visible. */
        *keep_admission = quarantine_take_over();
        s_manual_active = false;
        marker_sync();
        xSemaphoreGive(s_read_mtx);
    } else {
        /* Unreachable with portMAX_DELAY on FreeRTOS; fail closed anyway. */
        if (restore) s_dpdm_dirty = true;
        *keep_admission = quarantine_take_over();
        s_manual_active = false;
        if (result == ESP_OK) result = ESP_ERR_TIMEOUT;
        out->cleared = out->cleared && !restore;
    }
    return result;
}

esp_err_t mp2731_redetect_auto(int set, power_redetect_auto_t *out)
{
    if (out == NULL || set < -1 || set > 1) return ESP_ERR_INVALID_ARG;
    if (s_read_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (set == 0) {
        crd_set_disabled(&s_crd, CRD_DISABLED_OPERATOR);
    } else if (set == 1) {
        /* Refuse while BG_EN or USB_DET_EN is not proven back at 0: an
         * operator cannot switch the pulses back on over a charger they would
         * leave disabled or stuck in forced detection. */
        if (charger_dirty()) err = ESP_ERR_INVALID_STATE;
        else crd_set_disabled(&s_crd, CRD_ENABLED);
    }
    if (set != -1 && err == ESP_OK) {
        ESP_LOGW(TAG, "charger re-detect: automatic re-detection %s by operator (RAM only)",
                 set ? "ON" : "OFF");
    }
    const int64_t now = uptime_ms();
    *out = (power_redetect_auto_t){
        .enabled = s_crd.disabled == CRD_ENABLED,
        .state = crd_state_name(&s_crd, now),
        .disabled_reason = crd_disabled_name(s_crd.disabled),
        .last_outcome = crd_outcome_name(s_crd.last_outcome),
        .attempts = s_crd.attempts, .charging = s_crd.charging,
        .next_in_s = s_crd.has_next && s_crd.next_allowed_ms > now
                         ? (uint32_t)((s_crd.next_allowed_ms - now + 999) / 1000) : 0u,
    };
    xSemaphoreGive(s_read_mtx);
    return err;
}

esp_err_t mp2731_get_aico(power_aico_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready || s_read_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) return ESP_ERR_TIMEOUT;
    aico_io_t io;
    const esp_err_t err = aico_io(AICO_OP_READ, &io);
    if (err == ESP_OK) aico_fill(out, &io, uptime_ms());
    xSemaphoreGive(s_read_mtx);
    return err;
}

esp_err_t mp2731_set_aico(bool enabled, power_aico_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready || s_read_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (!mutation_begin()) return ESP_ERR_NOT_ALLOWED;
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) {
        mutation_end();
        return ESP_ERR_TIMEOUT;
    }
    aico_io_t io;
    const esp_err_t err = aico_io(enabled ? AICO_OP_SET_ON : AICO_OP_SET_OFF, &io);
    mutation_end();
    const int64_t now = uptime_ms();
    if (err == ESP_OK) {
        /* REG02 is written and verified: record it even if a status read
         * after it failed, or the next power read would re-arm at once. */
        aico_note_enabled(&s_aico, now, enabled);
        aico_fill(out, &io, now);
        ESP_LOGW(TAG, "AICO %s by operator: REG02 0x%02X (RAM-only charger register; "
                      "re-armed every %u min while on)", enabled ? "ON" : "OFF", io.r02,
                 (unsigned)(AICO_RESET_INTERVAL_MS / 60000));
    }
    xSemaphoreGive(s_read_mtx);
    return err;
}

esp_err_t mp2731_reset_aico(power_aico_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready || s_read_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (s_redetect_veto != NULL && s_redetect_veto()) return ESP_ERR_NOT_ALLOWED;
    if (!mutation_begin()) return ESP_ERR_NOT_ALLOWED;
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) {
        mutation_end();
        return ESP_ERR_TIMEOUT;
    }
    if (aico_vetoed()) {     /* a re-detect is in progress or being judged */
        xSemaphoreGive(s_read_mtx);
        mutation_end();
        return ESP_ERR_TIMEOUT;
    }
    aico_io_t io;
    const esp_err_t err = aico_io(AICO_OP_RESET, &io);
    mutation_end();
    const int64_t now = uptime_ms();
    /* ESP_OK = REG00 written and verified, whatever the status reads after. */
    aico_note_result(&s_aico, now, err == ESP_OK ? AICO_RESULT_RESET : AICO_RESULT_FAILED);
    if (err == ESP_OK) {
        aico_fill(out, &io, now);
        if (io.status_ok) {
            ESP_LOGW(TAG, "AICO manual re-arm: REG00 0x%02X rewritten (verified), IIN_DPM %u -> %u mA",
                     io.r00, iin_dpm_ma(io.r14_before), iin_dpm_ma(io.r14));
        } else {
            ESP_LOGW(TAG, "AICO manual re-arm: REG00 0x%02X rewritten (verified); status read after it failed",
                     io.r00);
        }
    }
    xSemaphoreGive(s_read_mtx);
    return err;
}
