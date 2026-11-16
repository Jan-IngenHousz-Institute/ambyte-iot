#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mp2731_diagnostics.h"

typedef struct { int fail_reg; unsigned reads; uint8_t registers[24]; } chip_t;
static esp_err_t read_reg(void *ctx, uint8_t reg, uint8_t *out)
{
    chip_t *c = ctx;
    c->reads++;
    if (reg == c->fail_reg) return ESP_FAIL;
    *out = c->registers[reg];
    return ESP_OK;
}
static void observe(mp2731_history_t *h, int64_t ms, esp_err_t err, uint8_t status,
                    uint8_t fault, uint16_t mv)
{
    power_reading_t p = {.battery_mv = mv};
    mp2731_frame_t f = {.status_valid = err != ESP_FAIL, .status = status, .fault = fault, .vbat = (uint8_t)(mv / 20u)};
    mp2731_history_observe(h, ms, err, &p, &f);
}
int main(void)
{
    mp2731_history_t h = {0};
    charger_diagnostics_t d;
    observe(&h, 1000, ESP_OK, 0x30, 0x80, 3900);
    observe(&h, 6000, ESP_OK, 0x30, 0x80, 3920);
    observe(&h, 11000, ESP_OK, 0x03, 0x86, 0); /* hot + thermal regulation, stopped */
    mp2731_history_snapshot(&h, &d, 12000);
    assert(d.charge_stops == 1 && d.charging_ms == 10000 && d.observed_ms == 10000);
    assert(d.adc_changes == 2 && d.battery_min_mv == 3900 && d.battery_max_mv == 3920);
    assert(d.last_stop_ms == 11000 && d.last_stop_fault == 0x86 && d.status_or == 0x33);
    uint64_t old_window = d.window_id;
    /* Failed submission: the next snapshot retains the range. */
    observe(&h, 16000, POWER_ERR_NO_INPUT, 0, 0xA0, 1000);
    mp2731_history_snapshot(&h, &d, 17000);
    assert(d.battery_min_mv == 3900 && d.battery_max_mv == 3920);
    mp2731_history_ack(&h, old_window); /* cannot erase the newer snapshot */
    assert(h.pending_range_valid);
    observe(&h, 18000, ESP_OK, 0x30, 0x80, 3880); /* arrives after snapshot */
    mp2731_history_ack(&h, d.window_id);
    mp2731_history_snapshot(&h, &d, 19000);
    assert(d.window_start_ms == 17000 && d.battery_min_mv == 3880 && d.battery_max_mv == 3880);
    mp2731_history_ack(&h, d.window_id);
    mp2731_history_snapshot(&h, &d, 20000);
    assert(!d.battery_range_valid);
    /* Unknown state / bus errors and long gaps cannot manufacture stops or duration. */
    observe(&h, 23000, ESP_FAIL, 0, 0, 0);
    observe(&h, 28000, ESP_OK, 0, 0, 3850);
    observe(&h, 33000, ESP_OK, 0x30, 0, 3860);
    observe(&h, 100000, ESP_OK, 0, 0, 3870);
    mp2731_history_snapshot(&h, &d, 100001);
    assert(d.charge_stops == 1 && d.unobserved_ms >= 77000 && d.read_errors == 2);

    chip_t chip = {.fail_reg = -1, .registers = {[0]=0x48, [1]=6, [2]=0xDC,
        [4]=0x55, [8]=0x8D, [0x0C]=3, [0x0D]=0x86, [0x10]=102, [0x14]=0xC8, [0x17]=0x80}};
    mp2731_bus_t bus = {.read = read_reg, .ctx = &chip}; /* no write port supplied */
    assert(mp2731_diagnostics_read(&bus, &d) == ESP_OK);
    assert(chip.reads == 24 && d.read_mask == 0xFFFFFFu && d.registers[23] == 0x80);
    char json[CHARGER_DIAGNOSTICS_JSON_CAP];
    charger_diagnostics_t worst = d;
    worst.reads = worst.read_errors = worst.status_reads = worst.adc_changes = UINT64_MAX;
    worst.charging_ms = worst.observed_ms = worst.unobserved_ms = worst.charge_stops = UINT64_MAX;
    worst.window_id = UINT64_MAX;
    worst.scan_start_ms = worst.scan_end_ms = worst.last_read_ms = worst.last_adc_change_ms = INT64_MAX;
    worst.last_stop_ms = worst.window_start_ms = worst.window_end_ms = INT64_MAX;
    worst.last_stop_valid = worst.battery_range_valid = true;
    worst.battery_min_mv = worst.battery_max_mv = UINT16_MAX;
    worst.last_stop_status = worst.last_stop_fault = worst.status_or = worst.fault_or = UINT8_MAX;
    assert(mp2731_diagnostics_render(&worst, json, sizeof json) > 0);
    printf("MAXIMUM=%s\n", json);
    assert(mp2731_diagnostics_render(&d, json, sizeof json) > 0);
    printf("FULL=%s\n", json);
    assert(mp2731_diagnostics_render(&d, json, 12) == -1 && json[0] == '\0');
    chip.fail_reg = 0x10; chip.reads = 0;
    assert(mp2731_diagnostics_read(&bus, &d) == ESP_FAIL);
    assert(chip.reads == 17 && d.read_mask == 0xFFFFu);
    assert(mp2731_diagnostics_render(&d, json, sizeof json) > 0);
    printf("PARTIAL=%s\n", json);
    chip.fail_reg = 0;
    assert(mp2731_diagnostics_read(&bus, &d) == ESP_FAIL);
    assert(mp2731_diagnostics_render(&d, json, sizeof json) > 0);
    printf("BUS_ERROR=%s\n", json);
    return 0;
}
