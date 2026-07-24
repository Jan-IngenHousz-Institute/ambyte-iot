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

typedef esp_err_t (*power_read_fn)(power_reading_t *out);

#ifdef __cplusplus
}
#endif

#endif
