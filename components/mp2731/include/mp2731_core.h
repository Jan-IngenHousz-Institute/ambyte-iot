#ifndef AMBYTE_MP2731_CORE_H
#define AMBYTE_MP2731_CORE_H

/* MP2731 read logic over a register-access seam. mp2731.c supplies the real I2C
 * bus; tests/power_policy_host.c supplies a fake chip that can be stuck, all-zero,
 * never-completing, I2C-erroring or healthy. No ESP-IDF driver includes here.
 *
 * Datasheet: MPS "MP2731 Rev. 1.1", 8/24/2020 (Farnell mirror
 * https://www.farnell.com/datasheets/3437344.pdf - the MPS site serves a bot
 * challenge). Page numbers below are the datasheet's own footers. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sensing_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* p.31 REG03 "ADC Control and OTG Configuration":
 *   bit7 ADC_START - "Set to 0 by default ... read-only when ADC_RATE = 1. This
 *                    bit stays high during ADC conversion." We use its fall as
 *                    the conversion-done signal (bench 2026-11-13: in charge mode
 *                    it reads 0 again well inside the old fixed 80 ms wait).
 *   bit6 ADC_RATE  - 0 one-shot, 1 continuous round-robin.
 *   bits5:0        - OTG voltage/current; preserved by every write.
 * Both ADC bits are reset by REG_RST and by the I2C watchdog (p.31 "Reset by
 * WTD: Y"). */
#define MP2731_REG_ADC_CTRL   0x03u
#define MP2731_ADC_START_BIT  0x80u
#define MP2731_ADC_RATE_BIT   0x40u

/* p.36 REG0C STATUS (live, read-only): VIN_STAT[7:5] (000 = no input, reset by
 * VIN POR), CHG_STAT[4:3] (00 not charging, 01 trickle, 10 CC, 11 done),
 * VSYS_STAT[0] (POR default 1). p.37 REG0D FAULT (live, "not latched"):
 * bit7 WATCHDOG_FAULT, bit5 INPUT_FAULT ("input OVP or no input"). */
#define MP2731_REG_STATUS     0x0Cu
#define MP2731_REG_FAULT      0x0Du
#define MP2731_STATUS_VIN(reg)    (((reg) >> 5) & 0x07u)
#define MP2731_STATUS_CHARGE(reg) (((reg) >> 3) & 0x03u)
#define MP2731_FAULT_INPUT        0x20u
#define MP2731_FAULT_WATCHDOG     0x80u

/* pp.37-39 ADC result registers: read-only, POR value 0x00, NOT reset by REG_RST
 * or the watchdog - so when no conversion runs they keep their last value (or
 * 0x00 since the chip's own power-on). Bit weights (0 offset):
 *   0x0E VBATT 20 mV · 0x0F VSYS 20 mV · 0x11 VIN 60 mV ·
 *   0x12 ICHG 17.5 mA (charge mode only) · 0x13 IIN 13.3 mA (charge mode only).
 * p.26 ADC: "available in charge mode and OTG mode" - there is NO battery-only
 * conversion. */
#define MP2731_REG_VBAT       0x0Eu
#define MP2731_REG_VSYS       0x0Fu
#define MP2731_REG_VIN        0x11u
#define MP2731_REG_ICHARGE    0x12u
#define MP2731_REG_IIN        0x13u

/* No conversion time is specified anywhere in the datasheet. The original
 * firmware waited a fixed 80 ms and that was always long enough in charge mode;
 * poll every 5 ms and give up after 250 ms (3x that) - a conversion that has
 * not finished by then is not going to (ADC unavailable or wedged). */
#define MP2731_ADC_POLL_MS        5u
#define MP2731_ADC_TIMEOUT_MS   250u

/* VIN at/above this counts as a real external source (unchanged; see mp2731.c
 * history: a 6 V-class panel delivering power sits at 5.0-7.0 V, a panel
 * floating at open circuit in twilight collapses to ~4.3 V). */
#define MP2731_VIN_MIN_MV      4700u

/* Plausibility. A frame outside these cannot describe a running unit:
 *   - VSYS powers the ESP32 reading it: below 2.5 V the 3V3 rail is gone
 *     (selftest_power uses the same floor); above 5.0 V is above anything the
 *     NVDC path regulates to (VSYS tracks VBATT + VTRACK, VBATT_REG ≤ 4.6 V).
 *   - VBATT above 4.8 V is above any single-cell regulation target. VBATT = 0
 *     is NOT rejected: with the charger idle and VIN present, the field reads
 *     VBATT 0 (memory 2026-09-29: 204 prod routes), and the gate does not use it.
 *   - an all-zero frame is the result registers' power-on value: no conversion
 *     has happened since the chip last reset (14:63:93:37:A3:FC, 2026-11-13). */
#define MP2731_VSYS_MIN_MV     2500u
#define MP2731_VSYS_MAX_MV     5000u
#define MP2731_VBAT_MAX_MV     4800u

/* Stale-frame signature: charge current in the ADC frame while the LIVE status
 * says "not charging". ICHG is a charge-mode-only channel (p.10), so a real
 * conversion cannot report current the chip is not delivering. Prod raw archive
 * 2026-11-08..13: 0 of ~14,000 heartbeats from healthy units show it; every one
 * of the eight 14:63:93:… Uluru units shows it on 85-100 % of its heartbeats,
 * each repeating only 1-2 distinct frames (e.g. 39:37:30: charge_ma 420 with
 * charge_status 0, ×181). Two LSBs, so a single-LSB ADC offset never trips it. */
#define MP2731_ICHG_STALE_MA     35u

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *val);
    esp_err_t (*write)(void *ctx, uint8_t reg, uint8_t val);
    void      (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} mp2731_bus_t;

/* Everything one read saw, for the CLI dump and the tests. */
typedef struct {
    uint8_t  ctrl_found;   /* REG03 as found before the read */
    uint8_t  status;       /* REG0C */
    uint8_t  fault;        /* REG0D */
    bool     status_valid; /* both live status/fault bytes completed, independent of ADC */
    uint8_t  vbat, vsys, vin, icc, iin;
    bool     converted;    /* ADC_START fell within MP2731_ADC_TIMEOUT_MS */
    uint32_t conv_ms;      /* polled time until it fell (or the timeout) */
} mp2731_frame_t;

/* One power read. Returns:
 *   ESP_OK                 - fresh, plausible frame in *out;
 *   POWER_ERR_NO_INPUT     - live status consistently says no input
 *                            (VIN_STAT 000 AND INPUT_FAULT AND not charging):
 *                            the ADC cannot convert (p.26), so the frame is
 *                            ignored - but this IS evidence of "on battery";
 *   POWER_ERR_ADC_TIMEOUT  - the conversion never completed;
 *   POWER_ERR_IMPLAUSIBLE  - a completed frame that cannot be real;
 *   any other error        - the bus failed.
 * On every non-OK return *out still holds the decoded (rejected) frame, for
 * diagnostics only. `frame` may be NULL. */
esp_err_t mp2731_core_read(const mp2731_bus_t *bus, power_reading_t *out,
                           mp2731_frame_t *frame);

/* Decode result registers into mV/mA (no validity judgement). */
void mp2731_core_decode(const mp2731_frame_t *f, power_reading_t *out);

/* True when a decoded frame can describe a running unit (limits above). */
bool mp2731_core_plausible(const power_reading_t *p);

/* Live status consistently says "no input" (see POWER_ERR_NO_INPUT). An
 * all-zero register file is NOT this (INPUT_FAULT would be 0). */
bool mp2731_core_no_input(uint8_t status, uint8_t fault);

/* ── Re-detection pulses (docs/charger-redetect.md) ────────────────────────
 * REG09 bit 3 BG_EN (POR 0, "Reset by REG_RST: Y, by WTD: Y"; bits 7:4 and
 * 2:0 reserved): "Setting this bit from 1 to 0 will reset the input plug-in
 * detection and safety timer" (REG09 table); "Writing the BG_EN bit from 1 to
 * 0 or re-inserting the input re-enables the input detection, clears the
 * safety timer fault and restarts the safety timer" (Safety Timer). The
 * register table names the 1 state "Disabled" (bandgap off): keep it for the
 * shortest possible time - set, read back, clear, read back, nothing between.
 *
 * REG0B bit 5 USB_DET_EN (POR 0, reset by WTD): "1: Force DP/DM detection";
 * the text adds "in the host mode". Unlike DM/DP (bits 4/3, "resets to 0
 * after 1 is written") the table does NOT say bit 5 self-clears, and this
 * fleet runs in the charger's default mode (I2C watchdog expired, REG0D bit
 * 7), so the chip may ignore it. Bits 4/3/0 are written 0 (writing 1 drives
 * DM/DP or pulls D+ to 0 V); bits 7:6 and 2:1 are preserved. */
#define MP2731_REG_BANDGAP     0x09u
#define MP2731_BG_EN_BIT       0x08u
#define MP2731_REG_USB_DET     0x0Bu
#define MP2731_USB_DET_EN_BIT  0x20u
#define MP2731_USB_DET_WRITE0  0x19u  /* DM, DP, FAST_CHG_RESET: never written 1 */
/* Extra clear attempts when BG_EN reads back 1 after the clear. */
#define MP2731_BG_EN_CLEAR_RETRIES 3u

typedef struct {
    uint8_t before;     /* register before the write */
    uint8_t set_rb;     /* readback after setting the bit */
    uint8_t after;      /* final readback */
    bool    latched;    /* the bit read back as 1 after setting it */
    bool    cleared;    /* final readback has the bit clear */
    uint8_t clear_writes;
    bool    set_attempted;  /* a write setting the bit was issued: it may have landed */
} mp2731_pulse_t;

/* BG_EN 0 -> 1 (verify) -> 0 (verify, with retries). Returns ESP_OK only when
 * the 1 latched AND the 0 read back: the 1->0 edge provably happened.
 * ESP_ERR_INVALID_RESPONSE = the 1 never latched (no edge, nothing written
 * after it); ESP_ERR_INVALID_STATE = BG_EN still 1 after every clear; any
 * other error = bus. On a bus error after the 1 latched the clear is still
 * attempted. The caller holds the bus for the whole sequence. */
esp_err_t mp2731_core_bg_en_pulse(const mp2731_bus_t *bus, mp2731_pulse_t *out);

/* USB_DET_EN := 1 with the rules above. ESP_OK when the write and readback
 * completed; `latched` tells whether the bit stayed 1 (0 = self-cleared or
 * ignored: indistinguishable from the register alone). */
esp_err_t mp2731_core_dpdm_force(const mp2731_bus_t *bus, mp2731_pulse_t *out);
/* If USB_DET_EN still reads 1, write it back to 0 (the POR value). *cleared
 * reports the final state; *wrote whether a write was needed. */
esp_err_t mp2731_core_dpdm_restore(const mp2731_bus_t *bus, uint8_t *final, bool *wrote);

#ifdef __cplusplus
}
#endif

#endif
