#include "mp2731.h"

#include <string.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#define TAG "mp2731"

/* Register map (subset we touch). REG03 holds the ADC control bits; REG0C is the
 * system-status register; REG0E..REG13 are the ADC result registers (one byte
 * each, refreshed after a conversion). */
#define MP2731_REG_ADC_CTRL   0x03
#define MP2731_REG_STATUS     0x0C
#define MP2731_REG_VBAT       0x0E
#define MP2731_REG_VSYS       0x0F
#define MP2731_REG_VIN        0x11
#define MP2731_REG_ICHARGE    0x12
#define MP2731_REG_IIN        0x13

/* REG03 bit7 = ADC_START (one-shot, self-clearing once the conversion ends). */
#define MP2731_ADC_START_BIT  (1U << 7)

/* REG0C status fields: bits[7:5] = VIN source status (0 = no input), bits[4:3] =
 * charge status (0 idle / 1 pre / 2 fast / 3 done). */
#define MP2731_STATUS_VIN(reg)    (((reg) >> 5) & 0x07U)
#define MP2731_STATUS_CHARGE(reg) (((reg) >> 3) & 0x03U)

/* VIN at/above this counts as a real external source. Picked from captured
 * field data: a 6 V-class panel delivering power sits at 5.0-7.0 V, while a
 * panel floating at open-circuit in twilight collapses to ~4.3 V (and can't
 * charge). 4.7 V sits in that gap — above the dusk float, below the lowest
 * real-source reading we've seen. NOTE: this is below the charger's own ~4.3 V
 * VIN_MIN comparator, so we deliberately don't trust the chip's VIN-status bit
 * here (it flickers at the margin); we threshold the ADC voltage instead. */
#define MP2731_VIN_MIN_MV   4700U

/* All ADC channels convert in well under this; the original firmware allowed
 * ~50 ms per pass. 80 ms is a comfortable margin for a fresh one-shot result. */
#define MP2731_ADC_CONV_MS    80

#define MP2731_I2C_TIMEOUT_TICKS pdMS_TO_TICKS(1000)

static bool s_ready = false;

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

esp_err_t mp2731_init(void)
{
    i2c_port_t port = I2C_NUM_MAX;
    esp_err_t err = i2c_bus_get_port(&port);
    if (err != ESP_OK) {
        s_ready = false;
        return err;
    }

    err = i2c_bus_probe_addr(port, MP2731_I2C_ADDR);
    s_ready = (err == ESP_OK);
    if (s_ready) {
        ESP_LOGI(TAG, "MP2731 charger detected @ 0x%02X", MP2731_I2C_ADDR);
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

esp_err_t mp2731_read_power(power_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Kick a one-shot ADC conversion (read-modify-write to preserve the other
     * charger-control bits in REG03), then wait for the result registers. */
    uint8_t ctrl = 0;
    esp_err_t err = mp2731_read_reg(MP2731_REG_ADC_CTRL, &ctrl);
    if (err != ESP_OK) {
        return err;
    }
    err = mp2731_write_reg(MP2731_REG_ADC_CTRL, ctrl | MP2731_ADC_START_BIT);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(MP2731_ADC_CONV_MS));

    uint8_t vbat = 0, vsys = 0, vin = 0, icc = 0, iin = 0, status = 0;
    if ((err = mp2731_read_reg(MP2731_REG_VBAT, &vbat)) != ESP_OK ||
        (err = mp2731_read_reg(MP2731_REG_VSYS, &vsys)) != ESP_OK ||
        (err = mp2731_read_reg(MP2731_REG_VIN, &vin)) != ESP_OK ||
        (err = mp2731_read_reg(MP2731_REG_ICHARGE, &icc)) != ESP_OK ||
        (err = mp2731_read_reg(MP2731_REG_IIN, &iin)) != ESP_OK ||
        (err = mp2731_read_reg(MP2731_REG_STATUS, &status)) != ESP_OK) {
        return err;
    }

    /* Datasheet ADC step sizes (matches the original firmware scaling):
     *   VBAT / VSYS : 20   mV / LSB
     *   VIN         : 60   mV / LSB
     *   ICHARGE     : 17.5 mA / LSB
     *   IIN         : 13.3 mA / LSB                                          */
    out->battery_mv = (uint16_t)(20 * vbat);
    out->system_mv  = (uint16_t)(20 * vsys);
    out->input_mv   = (uint16_t)(60 * vin);
    out->charge_ma  = (uint16_t)(175 * (uint16_t)icc / 10);
    out->input_ma   = (uint16_t)(133 * (uint16_t)iin / 10);

    /* External power present = the source is actually delivering usable power,
     * not floating at open-circuit in twilight. True when the battery is taking
     * real charge current (charge_ma, an analog reading we trust), OR when VIN
     * sits solidly above the dusk-float voltage (covers a full/idle battery that
     * is still externally powered, e.g. VIN 5 V with charge_ma 0). We do NOT key
     * off charge_status: it reports "fast charge" even at the dusk float where
     * charge_ma is 0. Used by the publish power gate. */
    out->charge_status = (uint8_t)MP2731_STATUS_CHARGE(status);
    out->input_present = (out->charge_ma > 0U) ||
                         (out->input_mv >= MP2731_VIN_MIN_MV);
    return ESP_OK;
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
