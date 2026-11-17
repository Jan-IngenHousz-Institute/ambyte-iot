#ifndef AMBYTE_MP2731_H
#define AMBYTE_MP2731_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "mp2731_core.h"
#include "sensing_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MP2731 single-cell battery charger / power-path manager (I2C @ 0x4B on the
 * shared platform bus). Provides the on-board power telemetry — battery, input
 * and system voltages plus charge / input currents — read from its internal
 * ADC. Mirrors the readings the original Ambyte firmware reported via
 * mp2731_readADC(). The platform I2C bus must be initialized before mp2731_init().
 */
#define MP2731_I2C_ADDR ((uint8_t)0x4B)

/* Probe the charger on the I2C bus. Returns ESP_OK and marks the driver ready
 * when the device ACKs at MP2731_I2C_ADDR; otherwise the driver stays "absent"
 * and mp2731_read_power() reports ESP_ERR_INVALID_STATE. Safe to call once at
 * boot after i2c_bus_init(). */
esp_err_t mp2731_init(void);
bool mp2731_is_ready(void);

/* One verified power read (see mp2731_core_read for the verdicts): ESP_OK only
 * for a fresh, plausible frame; POWER_ERR_NO_INPUT / _ADC_TIMEOUT / _IMPLAUSIBLE
 * or a bus error otherwise. ESP_ERR_INVALID_STATE if the charger is absent. */
esp_err_t mp2731_read_power(power_reading_t *out);

/* Diagnostics: the register frame and verdict of the most recent power read. */
esp_err_t mp2731_last_frame(mp2731_frame_t *out);

/* Diagnostics only (CLI `mp2731_regs`): one raw register read, no side effects.
 * Works whenever the I2C bus is up, ready or not. */
#define MP2731_REG_DUMP_COUNT 0x18u   /* REG00..REG17, includes safety timer */

/* Read-only heartbeat diagnostics, including partial register scans. Returns a
 * complete JSON object length or -1. Ack only after storage/submission succeeds. */
#define MP2731_DIAGNOSTICS_JSON_CAP 2048u
int mp2731_render_diagnostics(char *out, size_t cap, uint64_t *window_id, bool seal);
void mp2731_ack_diagnostics(uint64_t window_id);
esp_err_t mp2731_read_raw(uint8_t reg, uint8_t *out);

/* Diagnostics only (CLI `mp2731_regs probe`): start one conversion and poll
 * REG03 back-to-back. *first_ctrl = REG03 on the first read after the start
 * write (bit7 set = the chip latched ADC_START); *done_us = time until bit7
 * fell, UINT32_MAX if it never did within MP2731_ADC_TIMEOUT_MS. */
esp_err_t mp2731_probe_conversion(uint8_t *first_ctrl, uint32_t *done_us);
/* Read/set REG01 VIN_MIN (input voltage limit): 3700..15200 mV in 100 mV
 * steps. out is required. Set keeps the register-reset bit clear and verifies
 * the written value under the same I2C lock. No NVS/OTP write or boot override:
 * this changes the live charger register only. A charger reset can restore its
 * OTP default; an ESP reboot alone need not reset the charger. */
esp_err_t mp2731_get_vin_limit(power_vin_limit_t *out);
esp_err_t mp2731_set_vin_limit(uint16_t mv, power_vin_limit_t *out);
/* Read/set REG00 IIN_LIM (input current limit REGISTER): 100..3250 mA in
 * 50 mA steps. out is required. Set is one read-modify-write-readback under a
 * single I2C bus hold that keeps EN_HIZ (bit 7) and EN_ILIM (bit 6) exactly as
 * read; it succeeds only when the full byte reads back as written. With
 * EN_ILIM set the ILIM-pin resistor may cap the input below the register.
 * Live register only, like the VIN limit (docs/charger-iin-limit.md). */
esp_err_t mp2731_get_iin_limit(power_iin_limit_t *out);
esp_err_t mp2731_set_iin_limit(uint16_t ma, power_iin_limit_t *out);

/* Charger input re-detection (docs/charger-redetect.md, mp2731_redetect.h).
 * Manual: one BG_EN pulse (REG09 bit 3, 1 then 0, both verified) or one forced
 * DP/DM detection (REG0B bit 5, restored to 0 if the chip leaves it set),
 * with REG00..REG17 scanned before, immediately after, at +1.5 s and +5 s.
 * Blocks the caller ~5 s (the read mutex is released while waiting). Returns
 * ESP_ERR_NOT_ALLOWED under the maintenance lock, ESP_ERR_TIMEOUT when busy,
 * ESP_ERR_INVALID_RESPONSE when BG_EN=1 never latched (no 1->0 edge),
 * ESP_ERR_INVALID_STATE when BG_EN stayed 1 (cleared again on later reads).
 * Writes nothing else; no NVS, no OTP. */
esp_err_t mp2731_charger_redetect(power_redetect_method_t method, power_redetect_report_t *out);
/* Automatic re-detection switch: set -1 query, 0 off, 1 on. RAM only. */
esp_err_t mp2731_redetect_auto(int set, power_redetect_auto_t *out);
/* Maintenance-lock probe (OTA / AMBIT flash): vetoes automatic and manual
 * pulses. Set once by the composition root; NULL = never vetoed. */
void mp2731_redetect_set_veto(bool (*veto)(void));

/* Automatic Input Current Optimizer (docs/charger-aico.md, mp2731_aico.h).
 * get: REG02/00/14/15 under one bus hold plus the reset counters.
 * set: read-modify-write of REG02 changing ONLY bit 0 (bits 7:1 copied from
 * the read), verified readback; switching on restarts the reset period.
 * reset: rewrite REG00 with its own value now (bits 7:6 included), verified,
 * counted as a reset; refused under the maintenance lock (ESP_ERR_NOT_ALLOWED)
 * and while a manual re-detect is in progress (ESP_ERR_TIMEOUT).
 * The automatic hourly reset rides on mp2731_read_power and shares its veto.
 * Live registers only: no NVS, no OTP, no boot override. */
esp_err_t mp2731_get_aico(power_aico_t *out);
esp_err_t mp2731_set_aico(bool enabled, power_aico_t *out);
esp_err_t mp2731_reset_aico(power_aico_t *out);

/* Port adapter for the device_commands composition root. */
power_read_fn mp2731_get_power_read_fn(void);

#ifdef __cplusplus
}
#endif

#endif
