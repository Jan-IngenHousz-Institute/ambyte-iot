#include "mp2731_diagnostics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Existing read callers normally sample every 5–15 s. A longer gap or a bus
 * error breaks continuity; never turn hours offline into hours charging. */
#define HISTORY_MAX_GAP_MS 15000

static void range_add(bool *valid, uint16_t *lo, uint16_t *hi, uint16_t mv)
{
    if (!*valid || mv < *lo) *lo = mv;
    if (!*valid || mv > *hi) *hi = mv;
    *valid = true;
}

void mp2731_history_observe(mp2731_history_t *h, int64_t now_ms,
                            esp_err_t result, const power_reading_t *p,
                            const mp2731_frame_t *f)
{
    h->totals.reads++;
    h->totals.last_read_ms = now_ms;
    if (result != ESP_OK) h->totals.read_errors++;
    const bool status_valid = result == ESP_OK || result == POWER_ERR_NO_INPUT ||
        result == POWER_ERR_ADC_TIMEOUT || result == POWER_ERR_IMPLAUSIBLE;
    const int64_t dt = now_ms - h->previous_ms;
    const bool continuous = status_valid && h->previous_status_valid &&
                            dt >= 0 && dt <= HISTORY_MAX_GAP_MS;
    if (dt > 0) {
        if (continuous) {
            h->totals.observed_ms += (uint64_t)dt;
            if (h->previous_charge == 1u || h->previous_charge == 2u)
                h->totals.charging_ms += (uint64_t)dt;
        } else {
            h->totals.unobserved_ms += (uint64_t)dt;
        }
    }
    if (status_valid) {
        const uint8_t charge = (f->status >> 3) & 3u;
        h->totals.status_reads++;
        h->totals.status_or |= f->status;
        h->totals.fault_or |= f->fault;
        if (continuous && (h->previous_charge == 1u || h->previous_charge == 2u) &&
            (charge == 0u || charge == 3u)) {
            h->totals.charge_stops++;
            h->totals.last_stop_valid = true;
            h->totals.last_stop_ms = now_ms;
            h->totals.last_stop_status = f->status;
            h->totals.last_stop_fault = f->fault;
        }
        h->previous_charge = charge;
    }
    h->previous_status_valid = status_valid;
    h->previous_ms = now_ms;
    if (result == ESP_OK) {
        const uint8_t adc[] = {f->vbat, f->vsys, f->vin, f->icc, f->iin};
        const bool changed = !h->previous_adc_valid || memcmp(adc, h->previous_adc, sizeof adc) != 0;
        if (changed) {
            if (h->previous_adc_valid) h->totals.adc_changes++;
            h->totals.last_adc_change_ms = now_ms;
        }
        memcpy(h->previous_adc, adc, sizeof adc);
        h->previous_adc_valid = true;
        /* VBAT=0 is permitted by the legacy power reader but is not a battery
         * voltage minimum. Ranges are reported ADC values, not proof of freshness. */
        if (p->battery_mv > 0u)
            range_add(&h->active_range_valid, &h->active_min_mv, &h->active_max_mv,
                      p->battery_mv);
    }
}

void mp2731_history_snapshot(mp2731_history_t *h, charger_diagnostics_t *out,
                             int64_t now_ms)
{
    if (h->active_range_valid) {
        range_add(&h->pending_range_valid, &h->pending_min_mv, &h->pending_max_mv,
                  h->active_min_mv);
        range_add(&h->pending_range_valid, &h->pending_min_mv, &h->pending_max_mv,
                  h->active_max_mv);
        h->active_range_valid = false;
    }
    h->totals.window_id++;
    h->totals.window_end_ms = now_ms;
    *out = h->totals;
    out->window_start_ms = h->window_start_ms;
    out->window_end_ms = now_ms;
    out->battery_range_valid = h->pending_range_valid;
    out->battery_min_mv = h->pending_min_mv;
    out->battery_max_mv = h->pending_max_mv;
}

void mp2731_history_ack(mp2731_history_t *h, uint64_t window_id)
{
    /* An older concurrent reporter must not erase a newer snapshot's range.
     * Samples acquired after the snapshot live in active_* and are retained. */
    if (window_id == h->totals.window_id) {
        h->pending_range_valid = false;
        h->window_start_ms = h->totals.window_end_ms;
    }
}

esp_err_t mp2731_diagnostics_read(const mp2731_bus_t *bus, charger_diagnostics_t *out)
{
    out->read_mask = 0;
    memset(out->registers, 0, sizeof out->registers);
    for (uint8_t reg = 0; reg < CHARGER_REGISTER_COUNT; reg++) {
        uint8_t value = 0;
        esp_err_t err = bus->read(bus->ctx, reg, &value);
        if (err != ESP_OK) return out->read_error = err;
        out->registers[reg] = value;
        out->read_mask |= 1u << reg;
    }
    out->read_error = ESP_OK;
    return ESP_OK;
}

static void append(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    if (*used >= cap) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + *used, cap - *used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap - *used) { *used = cap; return; }
    *used += (size_t)n;
}

int mp2731_diagnostics_render(const charger_diagnostics_t *d, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0) return -1;
    append(out, cap, &n, "{\"scan_start_ms\":%lld,\"scan_end_ms\":%lld,"
           "\"read_mask\":%u,\"read_error\":%d,\"adc_freshness\":\"unverified\",\"registers\":{",
           (long long)d->scan_start_ms, (long long)d->scan_end_ms,
           (unsigned)d->read_mask, d->read_error);
    bool comma = false;
    for (unsigned r = 0; r < CHARGER_REGISTER_COUNT; r++) {
        if (!(d->read_mask & (1u << r))) continue;
        append(out, cap, &n, "%s\"%02X\":%u", comma ? "," : "", r, d->registers[r]);
        comma = true;
    }
    append(out, cap, &n, "}");
    if (d->read_mask & (1u << 0x00))
        append(out, cap, &n, ",\"iin_limit_ma\":%u,\"external_ilim_enabled\":%s,\"high_impedance\":%s",
               100u + 50u * (d->registers[0] & 0x3fu),
               d->registers[0] & 0x40u ? "true" : "false",
               d->registers[0] & 0x80u ? "true" : "false");
    if (d->read_mask & (1u << 0x01))
        append(out, cap, &n, ",\"vin_min_mv\":%u", 3700u + 100u * (d->registers[1] & 0x7fu));
    if (d->read_mask & (1u << 0x02))
        append(out, cap, &n, ",\"charge_ntc_enabled\":%s,\"aico_enabled\":%s",
               d->registers[2] & 0x10u ? "true" : "false", d->registers[2] & 1u ? "true" : "false");
    if (d->read_mask & (1u << 0x04))
        append(out, cap, &n, ",\"charge_config\":%u", (d->registers[4] >> 4) & 3u);
    if (d->read_mask & (1u << 0x08))
        append(out, cap, &n, ",\"safety_timer_enabled\":%s", d->registers[8] & 1u ? "true" : "false");
    if (d->read_mask & (1u << 0x10))
        /* REG10 is percent of the NTC reference, not degrees or absolute mV. */
        append(out, cap, &n, ",\"ntc_pct\":%.3f", 0.392 * d->registers[0x10]);
    if (d->read_mask & (1u << 0x17))
        append(out, cap, &n, ",\"safety_timer_expired\":%s", d->registers[0x17] & 0x80u ? "true" : "false");
    if (d->read_mask & (1u << 0x0C))
        append(out, cap, &n, ",\"thermal_regulation\":%s,\"ntc_float\":%s",
               d->registers[0x0C] & 2u ? "true" : "false", d->registers[0x0C] & 4u ? "true" : "false");
    if (d->read_mask & (1u << 0x0D))
        append(out, cap, &n, ",\"ntc_fault\":%u,\"thermal_shutdown\":%s",
               d->registers[0x0D] & 7u, d->registers[0x0D] & 0x10u ? "true" : "false");
    if (d->read_mask & (1u << 0x14))
        append(out, cap, &n, ",\"vin_limited\":%s,\"iin_limited\":%s",
               d->registers[0x14] & 0x80u ? "true" : "false", d->registers[0x14] & 0x40u ? "true" : "false");
    append(out, cap, &n, ",\"history\":{\"last_read_ms\":%lld,\"last_adc_change_ms\":%lld,\"reads\":%llu,\"read_errors\":%llu,\"status_reads\":%llu,"
           "\"adc_changes\":%llu,\"charging_ms\":%llu,\"observed_ms\":%llu,\"unobserved_ms\":%llu,"
           "\"charge_stops\":%llu,\"status_or\":%u,\"fault_or\":%u,\"last_stop\":",
           (long long)d->last_read_ms, (long long)d->last_adc_change_ms,
           (unsigned long long)d->reads, (unsigned long long)d->read_errors,
           (unsigned long long)d->status_reads, (unsigned long long)d->adc_changes,
           (unsigned long long)d->charging_ms, (unsigned long long)d->observed_ms,
           (unsigned long long)d->unobserved_ms, (unsigned long long)d->charge_stops,
           d->status_or, d->fault_or);
    if (d->last_stop_valid)
        append(out, cap, &n, "{\"uptime_ms\":%lld,\"status_reg\":%u,\"fault_reg\":%u}",
               (long long)d->last_stop_ms, d->last_stop_status, d->last_stop_fault);
    else append(out, cap, &n, "null");
    append(out, cap, &n, "},\"battery_window\":{\"id\":%llu,\"start_ms\":%lld,\"end_ms\":%lld,\"min_mv\":",
           (unsigned long long)d->window_id, (long long)d->window_start_ms, (long long)d->window_end_ms);
    if (d->battery_range_valid) append(out, cap, &n, "%u,\"max_mv\":%u", d->battery_min_mv, d->battery_max_mv);
    else append(out, cap, &n, "null,\"max_mv\":null");
    append(out, cap, &n, "}}");
    if (n >= cap) { out[0] = '\0'; return -1; }
    return (int)n;
}
