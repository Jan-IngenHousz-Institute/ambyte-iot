#ifndef AMBYTE_MP2731_DIAGNOSTICS_H
#define AMBYTE_MP2731_DIAGNOSTICS_H

#include <stddef.h>
#include "charger_diagnostics_port.h"
#include "mp2731_core.h"

/* Caller serializes this RAM-only tracker with the driver's short history critical section. */
typedef struct {
    charger_diagnostics_t totals;
    bool previous_status_valid, previous_adc_valid;
    uint8_t previous_charge;
    int64_t previous_ms;
    uint8_t previous_adc[5];
    bool active_range_valid, pending_range_valid;
    uint16_t active_min_mv, active_max_mv, pending_min_mv, pending_max_mv;
    int64_t window_start_ms;
} mp2731_history_t;

void mp2731_history_observe(mp2731_history_t *h, int64_t now_ms,
                            esp_err_t result, const power_reading_t *p,
                            const mp2731_frame_t *f);
void mp2731_history_snapshot(mp2731_history_t *h, charger_diagnostics_t *out,
                             int64_t now_ms);
void mp2731_history_peek(const mp2731_history_t *h, charger_diagnostics_t *out, int64_t now_ms);
void mp2731_history_ack(mp2731_history_t *h, uint64_t window_id);
/* Stops on the first bus failure to bound heartbeat latency on a dead bus.
 * Partial bytes retain their read mask. No writes, delays or ADC triggers. */
esp_err_t mp2731_diagnostics_read(const mp2731_bus_t *bus, charger_diagnostics_t *out);
int mp2731_diagnostics_render(const charger_diagnostics_t *d, char *out, size_t cap);

#endif
