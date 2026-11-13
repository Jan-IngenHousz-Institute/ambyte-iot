#ifndef AMBYTE_MP2731_H
#define AMBYTE_MP2731_H

#include <stdbool.h>
#include <stdint.h>

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
#define MP2731_REG_DUMP_COUNT 0x17u   /* REG00..REG16 */
esp_err_t mp2731_read_raw(uint8_t reg, uint8_t *out);

/* Port adapter for the device_commands composition root. */
power_read_fn mp2731_get_power_read_fn(void);

#ifdef __cplusplus
}
#endif

#endif
