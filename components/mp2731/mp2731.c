#include "mp2731.h"

#include <string.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "mp2731_core.h"

#define TAG "mp2731"

/* Register map, datasheet citations and the read algorithm live in
 * mp2731_core.{h,c} (host-testable); this file is only the I2C adapter. */

#define MP2731_I2C_TIMEOUT_TICKS pdMS_TO_TICKS(1000)

static bool s_ready = false;

/* One power read is a multi-transaction sequence (clear, start, poll, read).
 * Four tasks call it (publish gate, SD guard, heartbeat, CLI); interleaved, one
 * caller's "clear START" would abort another's conversion and surface as a
 * spurious adc_timeout. Serialise whole sequences, not just bus transactions. */
static SemaphoreHandle_t s_read_mtx;
static mp2731_frame_t    s_last_frame;    /* diagnostics (CLI), under s_read_mtx */
static esp_err_t         s_last_err = ESP_ERR_INVALID_STATE;

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

    if (s_read_mtx == NULL) {
        s_read_mtx = xSemaphoreCreateMutex();
        if (s_read_mtx == NULL) {
            s_ready = false;
            return ESP_ERR_NO_MEM;
        }
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

esp_err_t mp2731_read_power(power_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || s_read_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;   /* classified i2c_error: no evidence either way */
    }
    mp2731_frame_t f;
    const esp_err_t err = mp2731_core_read(&s_bus, out, &f);
    s_last_frame = f;
    s_last_err = err;
    xSemaphoreGive(s_read_mtx);
    return err;
}

esp_err_t mp2731_last_frame(mp2731_frame_t *out)
{
    if (out == NULL || s_read_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_read_mtx, MP2731_I2C_TIMEOUT_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_last_frame;
    const esp_err_t err = s_last_err;
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
