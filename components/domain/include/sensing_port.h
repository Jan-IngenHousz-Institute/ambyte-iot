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
} power_reading_t;

/* A power_read_fn returns ESP_OK ONLY for a fresh, plausible reading. Every other
 * return is "no power evidence" and is classified below; consumers must never
 * read *out as a measurement then (the driver may leave the raw, rejected frame
 * there for diagnostics). Two driver-specific codes carry the verdict:
 *   POWER_ERR_ADC_TIMEOUT  — the conversion never completed (a stuck/frozen ADC:
 *                            the result registers would be stale);
 *   POWER_ERR_IMPLAUSIBLE  — a completed frame that cannot describe a running
 *                            unit (all-zero, system rail below brown-out, out of
 *                            range). 2026-11-13: an all-zero frame held a prod
 *                            unit's publish gate closed for 9+ h.
 * ESP_ERR_INVALID_STATE = no charger; anything else is a bus error. */
#define POWER_ERR_ADC_TIMEOUT ESP_ERR_NOT_FINISHED
#define POWER_ERR_IMPLAUSIBLE ESP_ERR_INVALID_RESPONSE

typedef enum {
    POWER_READ_OK = 0,
    POWER_READ_ADC_TIMEOUT,
    POWER_READ_IMPLAUSIBLE,
    POWER_READ_I2C_ERROR,
    POWER_READ_UNAVAILABLE,   /* no charger fitted / driver not ready */
} power_read_reason_t;

static inline power_read_reason_t power_read_reason_from_err(esp_err_t err)
{
    if (err == ESP_OK) return POWER_READ_OK;
    if (err == POWER_ERR_ADC_TIMEOUT) return POWER_READ_ADC_TIMEOUT;
    if (err == POWER_ERR_IMPLAUSIBLE) return POWER_READ_IMPLAUSIBLE;
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
    }
    return "i2c_error";
}

typedef esp_err_t (*power_read_fn)(power_reading_t *out);

#ifdef __cplusplus
}
#endif

#endif
