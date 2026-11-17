#include "mp2731_core.h"

#include <stddef.h>
#include <string.h>

/* Pure MP2731 read logic - see mp2731_core.h for the register map and datasheet
 * citations. Field incident 2026-11-13 (Uluru), the reason this file exists:
 *
 * The original read did `REG03 |= ADC_START`, slept 80 ms and read 0x0E-0x13,
 * never checking that a conversion had happened. But the MP2731 ADC only runs in
 * charge or OTG mode (datasheet p.26) and its result registers are never cleared
 * (pp.37-39). So whenever the chip was not charging, the firmware re-published
 * the last converted frame - or, after the chip itself had reset, its power-on
 * 0x00s. On 2026-11-13 every 14:63:93:… Uluru unit reported "not charging" in
 * its live status on every heartbeat for the whole day (08:47-21:04 ACST) while
 * its ADC frame never changed, and units died during the day:
 *   - 14:63:93:37:A3:FC: all-zero frame (system_v 0 on a running unit) for 9+ h →
 *     "no input" → publish gate closed → 2,519 records held back;
 *   - 14:63:93:39:37:30 / 39:40:0C: one CHARGING frame (VIN 4.98 V) bit-identical
 *     for 10+ h across 30+ ESP resets → "input present" → gate open while the
 *     chip itself said it was not charging.
 * So this read now (1) makes a real 0→1 start edge, (2) watches ADC_START fall
 * (which catches a stuck-high bit, but cannot prove a conversion: on the bench
 * it reads low ~0.6 ms after the write either way), (3) rejects frames no
 * running unit can produce, and (4) consults the chip's LIVE status registers,
 * which do work without input, before believing any frame. Stale-but-plausible
 * frames remain indistinguishable here; consumers that need fresh evidence
 * (AMBIT flashing, the SD park guard) additionally require the frame to CHANGE
 * between reads (power_policy.c).
 *
 * One-shot, not continuous (ADC_RATE=1): in continuous mode ADC_START is
 * read-only, so freshness becomes unobservable - exactly the property that
 * failed in the field - and the un-kicked I2C watchdog (below) resets ADC_RATE
 * to one-shot 40 s later anyway.
 *
 * The chip's I2C watchdog (REG08[5:4], default 40 s, p.33) is deliberately left
 * alone: this firmware has never kicked it, so every unit runs in the charger's
 * "default mode" (bench 2026-11-13: REG0D = 0x80, WATCHDOG_FAULT set), and the
 * whole fleet's charging behaviour is calibrated on that. Expiry only resets
 * REG03's ADC bits between reads, and every read below sets them itself, so the
 * watchdog cannot freeze this path. Kicking or disabling it would move the
 * charger to "host mode" - a charging-behaviour change this fix does not need. */

static esp_err_t rd(const mp2731_bus_t *bus, uint8_t reg, uint8_t *val)
{
    return bus->read(bus->ctx, reg, val);
}

void mp2731_core_decode(const mp2731_frame_t *f, power_reading_t *out)
{
    out->battery_mv = (uint16_t)(20u * f->vbat);
    out->system_mv  = (uint16_t)(20u * f->vsys);
    out->input_mv   = (uint16_t)(60u * f->vin);
    out->charge_ma  = (uint16_t)(175u * (uint32_t)f->icc / 10u);
    out->input_ma   = (uint16_t)(133u * (uint32_t)f->iin / 10u);
    out->charge_status = (uint8_t)MP2731_STATUS_CHARGE(f->status);
    /* External power present = the source is actually delivering usable power,
     * not floating at open-circuit in twilight: real charge current, OR VIN
     * solidly above the dusk-float voltage (a full/idle battery still on
     * external power). Not keyed on CHG_STAT (reports "fast charge" at the dusk
     * float with charge_ma 0) nor on VIN_STAT (flickers at the margin).
     * Unchanged from the original driver: valid frames gate exactly as before. */
    out->input_present = (out->charge_ma > 0u) || (out->input_mv >= MP2731_VIN_MIN_MV);
    out->status_reg = f->status;
    out->fault_reg = f->fault;
}

bool mp2731_core_plausible(const power_reading_t *p)
{
    if (p->battery_mv == 0 && p->system_mv == 0 && p->input_mv == 0 &&
        p->charge_ma == 0 && p->input_ma == 0) {
        return false;                          /* the registers' power-on value */
    }
    if (p->system_mv < MP2731_VSYS_MIN_MV || p->system_mv > MP2731_VSYS_MAX_MV) {
        return false;                          /* we are running on this rail */
    }
    if (p->charge_ma >= MP2731_ICHG_STALE_MA && p->charge_status == 0u) {
        return false;                          /* charging per ADC, not per live status */
    }
    return p->battery_mv <= MP2731_VBAT_MAX_MV;
}

bool mp2731_core_no_input(uint8_t status, uint8_t fault)
{
    /* All three must agree. VIN_STAT alone is 000 at POR and during input
     * qualification; requiring INPUT_FAULT ("OVP or no input", live, p.37) and
     * CHG_STAT = not charging rejects an all-zero register file (INPUT_FAULT 0)
     * and any state in which the chip is demonstrably charging. */
    return MP2731_STATUS_VIN(status) == 0u &&
           (fault & MP2731_FAULT_INPUT) != 0u &&
           MP2731_STATUS_CHARGE(status) == 0u;
}

esp_err_t mp2731_core_read(const mp2731_bus_t *bus, power_reading_t *out,
                           mp2731_frame_t *frame)
{
    mp2731_frame_t f;
    memset(&f, 0, sizeof f);
    memset(out, 0, sizeof *out);

    uint8_t ctrl = 0;
    esp_err_t err = rd(bus, MP2731_REG_ADC_CTRL, &ctrl);
    if (err != ESP_OK) goto done;
    f.ctrl_found = ctrl;

    /* A real start edge. If ADC_START (or continuous mode) is already set, a
     * read-modify-write `ctrl | START` writes 1 over 1 and starts nothing - the
     * original driver's blind spot. Clear both ADC bits first, then set START
     * alone (one-shot). Bits 5:0 (OTG settings) are preserved verbatim. */
    const uint8_t base = (uint8_t)(ctrl & (uint8_t)~(MP2731_ADC_START_BIT | MP2731_ADC_RATE_BIT));
    if (ctrl != base) {
        err = bus->write(bus->ctx, MP2731_REG_ADC_CTRL, base);
        if (err != ESP_OK) goto done;
    }
    err = bus->write(bus->ctx, MP2731_REG_ADC_CTRL, (uint8_t)(base | MP2731_ADC_START_BIT));
    if (err != ESP_OK) goto done;

    /* Completion: ADC_START "stays high during ADC conversion" (p.31). */
    for (uint32_t waited = 0; waited < MP2731_ADC_TIMEOUT_MS; ) {
        bus->delay_ms(bus->ctx, MP2731_ADC_POLL_MS);
        waited += MP2731_ADC_POLL_MS;
        uint8_t now = 0;
        err = rd(bus, MP2731_REG_ADC_CTRL, &now);
        if (err != ESP_OK) goto done;
        f.conv_ms = waited;
        if ((now & MP2731_ADC_START_BIT) == 0u) {
            f.converted = true;
            break;
        }
    }
    if (!f.converted) {
        /* Leave START low so the next read produces a fresh edge again. */
        (void)bus->write(bus->ctx, MP2731_REG_ADC_CTRL, base);
    }

    if ((err = rd(bus, MP2731_REG_STATUS, &f.status)) != ESP_OK ||
        (err = rd(bus, MP2731_REG_FAULT, &f.fault)) != ESP_OK) goto done;
    f.status_valid = true;
    if ((err = rd(bus, MP2731_REG_VBAT, &f.vbat)) != ESP_OK ||
        (err = rd(bus, MP2731_REG_VSYS, &f.vsys)) != ESP_OK ||
        (err = rd(bus, MP2731_REG_VIN, &f.vin)) != ESP_OK ||
        (err = rd(bus, MP2731_REG_ICHARGE, &f.icc)) != ESP_OK ||
        (err = rd(bus, MP2731_REG_IIN, &f.iin)) != ESP_OK) {
        goto done;
    }
    mp2731_core_decode(&f, out);

    /* Verdict order matters:
     * 1. No input (live status) - the ADC cannot have converted (p.26), whatever
     *    ADC_START did, so the frame is stale by definition; but the absence of
     *    external power is itself solid evidence.
     * 2. No completion - stale frame.
     * 3. Completed but impossible. */
    if (mp2731_core_no_input(f.status, f.fault)) {
        out->input_present = false;
        err = POWER_ERR_NO_INPUT;
    } else if (!f.converted) {
        err = POWER_ERR_ADC_TIMEOUT;
    } else if (!mp2731_core_plausible(out)) {
        err = POWER_ERR_IMPLAUSIBLE;
    } else {
        err = ESP_OK;
    }

done:
    if (frame != NULL) *frame = f;
    return err;
}

esp_err_t mp2731_core_bg_en_pulse(const mp2731_bus_t *bus, mp2731_pulse_t *out)
{
    memset(out, 0, sizeof *out);
    uint8_t v = 0;
    esp_err_t err = rd(bus, MP2731_REG_BANDGAP, &v);
    if (err != ESP_OK) return err;
    out->before = v;
    /* Read-modify-write: the reserved bits keep whatever the chip holds. */
    out->set_attempted = true;
    err = bus->write(bus->ctx, MP2731_REG_BANDGAP, (uint8_t)(v | MP2731_BG_EN_BIT));
    if (err == ESP_OK) err = rd(bus, MP2731_REG_BANDGAP, &out->set_rb);
    if (err == ESP_OK) {
        out->latched = (out->set_rb & MP2731_BG_EN_BIT) != 0u;
        if (!out->latched) {
            /* Nothing to undo: the bit is 0. No 1->0 edge happened, so the
             * detection was NOT reset; never report that as a re-detect. */
            out->after = out->set_rb;
            out->cleared = true;
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    /* Clear even after a bus error: the write may have landed, and a BG_EN
     * left at 1 (bandgap "Disabled") is the one state this must not leave. */
    const uint8_t clear = (uint8_t)((err == ESP_OK ? out->set_rb : (v | MP2731_BG_EN_BIT)) &
                                    (uint8_t)~MP2731_BG_EN_BIT);
    esp_err_t clear_err = ESP_FAIL;
    for (uint8_t i = 0; i < 1u + MP2731_BG_EN_CLEAR_RETRIES; i++) {
        out->clear_writes++;
        clear_err = bus->write(bus->ctx, MP2731_REG_BANDGAP, clear);
        uint8_t rb = 0;
        if (clear_err == ESP_OK) clear_err = rd(bus, MP2731_REG_BANDGAP, &rb);
        if (clear_err == ESP_OK) {
            out->after = rb;
            if ((rb & MP2731_BG_EN_BIT) == 0u) { out->cleared = true; break; }
        }
    }
    if (err != ESP_OK) return err;             /* the set path failed */
    if (!out->cleared) return clear_err != ESP_OK ? clear_err : ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t mp2731_core_dpdm_force(const mp2731_bus_t *bus, mp2731_pulse_t *out)
{
    memset(out, 0, sizeof *out);
    uint8_t v = 0;
    esp_err_t err = rd(bus, MP2731_REG_USB_DET, &v);
    if (err != ESP_OK) return err;
    out->before = v;
    const uint8_t w = (uint8_t)((v & (uint8_t)~MP2731_USB_DET_WRITE0) | MP2731_USB_DET_EN_BIT);
    out->set_attempted = true;   /* from here the write may have landed: restore owed */
    err = bus->write(bus->ctx, MP2731_REG_USB_DET, w);
    if (err == ESP_OK) err = rd(bus, MP2731_REG_USB_DET, &out->set_rb);
    if (err != ESP_OK) return err;
    out->latched = (out->set_rb & MP2731_USB_DET_EN_BIT) != 0u;
    out->after = out->set_rb;
    out->cleared = !out->latched;
    return ESP_OK;
}

esp_err_t mp2731_core_dpdm_restore(const mp2731_bus_t *bus, uint8_t *final, bool *wrote)
{
    *wrote = false;
    uint8_t v = 0;
    esp_err_t err = rd(bus, MP2731_REG_USB_DET, &v);
    if (err != ESP_OK) return err;
    if ((v & MP2731_USB_DET_EN_BIT) != 0u) {
        *wrote = true;
        err = bus->write(bus->ctx, MP2731_REG_USB_DET,
                         (uint8_t)(v & (uint8_t)~(MP2731_USB_DET_EN_BIT | MP2731_USB_DET_WRITE0)));
        if (err == ESP_OK) err = rd(bus, MP2731_REG_USB_DET, &v);
        if (err != ESP_OK) return err;
    }
    *final = v;
    return (v & MP2731_USB_DET_EN_BIT) != 0u ? ESP_ERR_INVALID_STATE : ESP_OK;
}
