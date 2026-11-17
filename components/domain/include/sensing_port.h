#ifndef AMBYTE_SENSING_PORT_H
#define AMBYTE_SENSING_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temperature_c;
    float humidity_percent;
    float pressure_pa;
} measurement_t;

typedef esp_err_t (*sensor_read_fn)(measurement_t *out);
typedef esp_err_t (*clock_read_fn)(time_t *out);
/* Set the RTC (and system clock) from a UTC epoch. Injected like clock_read_fn so
 * the device_commands layer stays driver-agnostic. */
typedef esp_err_t (*clock_set_fn)(time_t epoch_utc);

/* On-board power telemetry from the MP2731 charger (mV / mA). */
typedef struct {
    uint16_t battery_mv;   /* battery voltage */
    uint16_t system_mv;    /* system rail voltage */
    uint16_t input_mv;     /* input (VBUS / panel) voltage */
    uint16_t charge_ma;    /* battery charge current */
    uint16_t input_ma;     /* input current */
    bool     input_present;/* external source detected (VIN status or voltage > UVLO) */
    uint8_t  charge_status;/* 0=idle 1=pre-charge 2=fast-charge 3=charge-done (REG0C) */
    /* Raw live charger status/fault registers (MP2731 REG0C / REG0D), filled on
     * EVERY read the bus completed - also when the frame was rejected - so
     * telemetry can say why a charger's ADC stopped (input, NTC, thermal, …). */
    uint8_t  status_reg;
    uint8_t  fault_reg;
} power_reading_t;

/* A power_read_fn returns ESP_OK ONLY for a fresh, plausible reading. Every other
 * return is "no power evidence" and is classified below; consumers must never
 * read *out as a measurement then (the driver may leave the raw, rejected frame
 * there for diagnostics). Two driver-specific codes carry the verdict:
 *   POWER_ERR_ADC_TIMEOUT  - the conversion never completed (a stuck/frozen ADC:
 *                            the result registers would be stale);
 *   POWER_ERR_IMPLAUSIBLE  - a completed frame that cannot describe a running
 *                            unit (all-zero, system rail below brown-out, out of
 *                            range). 2026-11-13: an all-zero frame held a prod
 *                            unit's publish gate closed for 9+ h.
 *   POWER_ERR_NO_INPUT     - the charger's live status says, consistently, that
 *                            there is no input. Its ADC only runs with an input
 *                            (MP2731 datasheet p.26), so there are no voltages -
 *                            but "no external power" itself is trustworthy: it is
 *                            reported (on_battery) and refuses AMBIT flashing;
 *                            it never stops publishing.
 * ESP_ERR_INVALID_STATE = no charger; anything else is a bus error. */
#define POWER_ERR_ADC_TIMEOUT ESP_ERR_NOT_FINISHED
#define POWER_ERR_IMPLAUSIBLE ESP_ERR_INVALID_RESPONSE
#define POWER_ERR_NO_INPUT    ESP_ERR_NOT_FOUND

typedef enum {
    POWER_READ_OK = 0,
    POWER_READ_ADC_TIMEOUT,
    POWER_READ_IMPLAUSIBLE,
    POWER_READ_I2C_ERROR,
    POWER_READ_UNAVAILABLE,   /* no charger fitted / driver not ready */
    POWER_READ_NO_INPUT,      /* no voltages, but live status: no external input */
} power_read_reason_t;

static inline power_read_reason_t power_read_reason_from_err(esp_err_t err)
{
    if (err == ESP_OK) return POWER_READ_OK;
    if (err == POWER_ERR_ADC_TIMEOUT) return POWER_READ_ADC_TIMEOUT;
    if (err == POWER_ERR_IMPLAUSIBLE) return POWER_READ_IMPLAUSIBLE;
    if (err == POWER_ERR_NO_INPUT) return POWER_READ_NO_INPUT;
    if (err == ESP_ERR_INVALID_STATE) return POWER_READ_UNAVAILABLE;
    return POWER_READ_I2C_ERROR;
}

/* Stable telemetry spelling (health.power.reason). Never rename a value. */
static inline const char *power_read_reason_name(power_read_reason_t r)
{
    switch (r) {
    case POWER_READ_OK:          return "ok";
    case POWER_READ_ADC_TIMEOUT: return "adc_timeout";
    case POWER_READ_IMPLAUSIBLE: return "implausible";
    case POWER_READ_I2C_ERROR:   return "i2c_error";
    case POWER_READ_UNAVAILABLE: return "unavailable";
    case POWER_READ_NO_INPUT:    return "no_input";
    }
    return "i2c_error";
}

typedef esp_err_t (*power_read_fn)(power_reading_t *out);

/* Charger input-voltage regulation threshold, distinct from measured VIN and
 * the firmware's external-power detection threshold. REG01 is returned so bench
 * scripts can compare the decoded limit with the actual hardware readback. */
#define POWER_VIN_LIMIT_MIN_MV  3700U
#define POWER_VIN_LIMIT_MAX_MV 15200U
#define POWER_VIN_LIMIT_STEP_MV  100U

static inline bool power_vin_limit_valid_mv(uint32_t mv)
{
    return mv >= POWER_VIN_LIMIT_MIN_MV && mv <= POWER_VIN_LIMIT_MAX_MV &&
           (mv - POWER_VIN_LIMIT_MIN_MV) % POWER_VIN_LIMIT_STEP_MV == 0;
}

typedef struct {
    uint16_t mv;
    uint8_t reg01;
} power_vin_limit_t;

typedef esp_err_t (*power_get_vin_limit_fn)(power_vin_limit_t *out);
typedef esp_err_t (*power_set_vin_limit_fn)(uint16_t mv, power_vin_limit_t *out);

/* Charger input current limit REGISTER (REG00[5:0] IIN_LIM), not the input
 * current itself and not the battery charge current. With EN_ILIM (REG00 bit 6,
 * set on every field unit: REG00=0x48) the chip uses the LOWER of this register
 * and the hardware ILIM-pin resistor, so the effective ceiling can sit below
 * the register value. HIZ (bit 7) turns the converter off entirely. Both bits
 * are reported, and a set preserves them exactly as read. */
#define POWER_IIN_LIMIT_MIN_MA   100U
#define POWER_IIN_LIMIT_MAX_MA  3250U
#define POWER_IIN_LIMIT_STEP_MA   50U
#define POWER_IIN_LIMIT_REG_MASK 0x3FU   /* REG00[5:0] */
#define POWER_IIN_EN_ILIM_BIT    0x40U   /* REG00 bit 6: ILIM pin limit enabled */
#define POWER_IIN_HIZ_BIT        0x80U   /* REG00 bit 7: EN_HIZ */

static inline bool power_iin_limit_valid_ma(uint32_t ma)
{
    return ma >= POWER_IIN_LIMIT_MIN_MA && ma <= POWER_IIN_LIMIT_MAX_MA &&
           (ma - POWER_IIN_LIMIT_MIN_MA) % POWER_IIN_LIMIT_STEP_MA == 0;
}

typedef struct {
    uint16_t ma;       /* register limit decoded from reg00[5:0] */
    uint8_t reg00;     /* full raw readback, bits 7:6 included */
} power_iin_limit_t;

typedef esp_err_t (*power_get_iin_limit_fn)(power_iin_limit_t *out);
typedef esp_err_t (*power_set_iin_limit_fn)(uint16_t ma, power_iin_limit_t *out);

/* Charger input re-detection (docs/charger-redetect.md). A manual pulse
 * reports the full REG00..REG17 file before, immediately after, and twice
 * after the write (detection settles), so the operator sees what the chip
 * did - not what the firmware hoped. RAM only: no NVS, no OTP, and only the
 * one bit of REG09 (bg_en) or REG0B (dpdm) is written. */
typedef enum {
    POWER_REDETECT_BG_EN = 0,   /* REG09 bit 3: 1 then 0, resets input detection */
    POWER_REDETECT_DPDM  = 1,   /* REG0B bit 5: force DP/DM detection */
} power_redetect_method_t;

#define POWER_CHARGER_REG_COUNT   24u   /* REG00..REG17 */
#define POWER_REDETECT_SNAPSHOTS   4u   /* before, immediate, +1.5 s, +5 s */
/* Settle points after the write. DP/DM detection is bounded by the 500 ms DCD
 * timer plus a 50 ms adapter timer and the primary/secondary comparisons
 * (tens of ms): 1.5 s is three DCD timeouts, so VIN_STAT has settled. The
 * datasheet gives no time for buck soft-start to CHG_STAT; 5 s (10x DCD)
 * leaves room for the converter to start and charging to report. */
#define POWER_REDETECT_SETTLE1_MS 1500
#define POWER_REDETECT_SETTLE2_MS 5000

typedef struct {
    int32_t  t_ms;    /* relative to the register write; the "before" scan is negative */
    uint32_t mask;    /* bit N = register N read; a scan stops at the first bus error */
    uint8_t  regs[POWER_CHARGER_REG_COUNT];
} power_charger_snapshot_t;

typedef struct {
    power_redetect_method_t method;
    uint8_t  reg;          /* 0x09 or 0x0B */
    uint8_t  reg_before;   /* register before the write */
    uint8_t  reg_set;      /* readback right after setting the bit */
    uint8_t  reg_after;    /* final readback (bg_en: after the clear; dpdm: after restore) */
    bool     latched;      /* the bit read back as 1 */
    bool     cleared;      /* the bit is 0 at the end */
    bool     restored;     /* dpdm: the chip left bit 5 set; firmware wrote it back to 0 */
    uint8_t  snapshots;    /* valid entries in snap[] */
    power_charger_snapshot_t snap[POWER_REDETECT_SNAPSHOTS];
} power_redetect_report_t;

typedef esp_err_t (*power_redetect_fn)(power_redetect_method_t method, power_redetect_report_t *out);

/* Automatic re-detection switch (RAM only, default on, back on at every boot). */
typedef struct {
    bool        enabled;
    const char *state;            /* static strings, see mp2731_redetect.h */
    const char *disabled_reason;
    const char *last_outcome;
    uint32_t    attempts, charging;
    uint32_t    next_in_s;
} power_redetect_auto_t;

/* set: -1 query, 0 disable, 1 enable. */
typedef esp_err_t (*power_redetect_auto_fn)(int set, power_redetect_auto_t *out);

/* Automatic Input Current Optimizer (docs/charger-aico.md). REG02 bit 0
 * AICO_EN is a live charger register (no NVS); the firmware re-arms it by
 * rewriting REG00 at most hourly (mp2731_aico.h). iin_limit_ma is the REG00
 * register limit, iin_dpm_ma the limit AICO currently applies (REG14[5:0]). */
typedef struct {
    bool     enabled;           /* REG02 bit 0 */
    bool     aico_stat;         /* REG15 bit 7: AICO action */
    uint8_t  reg02, reg00, reg14, reg15;
    uint16_t iin_limit_ma, iin_dpm_ma;
    uint32_t resets, failed;    /* per boot, automatic + manual */
    bool     has_reset;
    int64_t  last_reset_ms;     /* uptime of the last verified reset */
    uint32_t next_check_in_s;
} power_aico_t;

typedef esp_err_t (*power_get_aico_fn)(power_aico_t *out);
typedef esp_err_t (*power_set_aico_fn)(bool enabled, power_aico_t *out);
/* Rewrite REG00 with its own value now (re-arms AICO); counted as a reset. */
typedef esp_err_t (*power_reset_aico_fn)(power_aico_t *out);

#ifdef __cplusplus
}
#endif

#endif
