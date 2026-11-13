/* v2.16.0 (c1fbfcd) power read + publish gate, transplanted VERBATIM onto the
 * register seam so tests/power_policy_host.c can replay the 2026-11-13 Uluru
 * incident against the code that was in the field. Sources:
 *   components/mp2731/mp2731.c            mp2731_read_power()
 *   components/device_commands/device_commands.c  device_commands_publish_power_ok()
 * Only the I2C calls became bus->read/write, vTaskDelay became bus->delay_ms,
 * and now_ms()/the 5 s cache became the caller's clock (the harness calls once
 * per evaluation). Never "fix" this file: its whole purpose is to stay wrong in
 * exactly the way the field firmware was (the red half of a red/green test). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mp2731_core.h"

#define LEGACY_ADC_CONV_MS 80
#define LEGACY_VIN_MIN_MV  4700U
#define LEGACY_ON_DWELL_MS  15000
#define LEGACY_OFF_DWELL_MS 60000

esp_err_t legacy_read_power(const mp2731_bus_t *bus, power_reading_t *out)
{
    uint8_t ctrl = 0;
    esp_err_t err = bus->read(bus->ctx, 0x03, &ctrl);
    if (err != ESP_OK) {
        return err;
    }
    err = bus->write(bus->ctx, 0x03, ctrl | (1U << 7));
    if (err != ESP_OK) {
        return err;
    }
    bus->delay_ms(bus->ctx, LEGACY_ADC_CONV_MS);

    uint8_t vbat = 0, vsys = 0, vin = 0, icc = 0, iin = 0, status = 0;
    if ((err = bus->read(bus->ctx, 0x0E, &vbat)) != ESP_OK ||
        (err = bus->read(bus->ctx, 0x0F, &vsys)) != ESP_OK ||
        (err = bus->read(bus->ctx, 0x11, &vin)) != ESP_OK ||
        (err = bus->read(bus->ctx, 0x12, &icc)) != ESP_OK ||
        (err = bus->read(bus->ctx, 0x13, &iin)) != ESP_OK ||
        (err = bus->read(bus->ctx, 0x0C, &status)) != ESP_OK) {
        return err;
    }
    out->battery_mv = (uint16_t)(20 * vbat);
    out->system_mv  = (uint16_t)(20 * vsys);
    out->input_mv   = (uint16_t)(60 * vin);
    out->charge_ma  = (uint16_t)(175 * (uint16_t)icc / 10);
    out->input_ma   = (uint16_t)(133 * (uint16_t)iin / 10);
    out->charge_status = (uint8_t)((status >> 3) & 0x03U);
    out->input_present = (out->charge_ma > 0U) ||
                         (out->input_mv >= LEGACY_VIN_MIN_MV);
    return ESP_OK;
}

static bool    s_pub_gate_open    = false;
static bool    s_pub_gate_cand    = false;
static int64_t s_pub_gate_cand_ms = 0;

bool legacy_publish_power_ok(const mp2731_bus_t *bus, int64_t now)
{
    power_reading_t p;
    if (legacy_read_power(bus, &p) != ESP_OK) {
        /* Transient I2C/ADC failure: hold the last decision rather than flap. */
        return s_pub_gate_open;
    }
    const bool present = p.input_present;

    if (present == s_pub_gate_open) {
        s_pub_gate_cand = present;
    } else if (present != s_pub_gate_cand) {
        s_pub_gate_cand    = present;
        s_pub_gate_cand_ms = now;
    } else {
        const int64_t dwell = present ? LEGACY_ON_DWELL_MS : LEGACY_OFF_DWELL_MS;
        if ((now - s_pub_gate_cand_ms) >= dwell) {
            s_pub_gate_open = present;
        }
    }
    return s_pub_gate_open;
}
