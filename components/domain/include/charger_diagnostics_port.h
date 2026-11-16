#ifndef AMBYTE_CHARGER_DIAGNOSTICS_PORT_H
#define AMBYTE_CHARGER_DIAGNOSTICS_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define CHARGER_REGISTER_COUNT 24u /* MP2731 REG00..REG17, inclusive */
#define CHARGER_DIAGNOSTICS_JSON_CAP 2048u

/* Read-only evidence. ADC bytes, including NTC, may be stale: a register scan
 * does not start a conversion and is never an input to a power-control policy.
 * mask bit N means register N was read; a missing bit is not a zero reading. */
typedef struct {
    uint8_t registers[CHARGER_REGISTER_COUNT];
    uint32_t read_mask;
    int read_error;
    int64_t scan_start_ms, scan_end_ms;
    uint64_t reads, read_errors, status_reads, adc_changes;
    int64_t last_read_ms, last_adc_change_ms;
    uint64_t charging_ms, observed_ms, unobserved_ms, charge_stops;
    bool last_stop_valid;
    int64_t last_stop_ms;
    uint8_t last_stop_status, last_stop_fault;
    uint8_t status_or, fault_or;
    bool battery_range_valid;
    uint16_t battery_min_mv, battery_max_mv;
    int64_t window_start_ms, window_end_ms;
    uint64_t window_id;
} charger_diagnostics_t;


#endif
