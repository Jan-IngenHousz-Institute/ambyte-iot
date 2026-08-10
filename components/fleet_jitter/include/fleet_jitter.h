#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Map a six-byte Wi-Fi MAC address into [0, slot_count) with 32-bit FNV-1a.
 *
 * Keeping this operation separate from the hardware read makes the fleet slot
 * stable and directly testable without substituting another implementation of
 * the production hash. All arguments are required and slot_count must be
 * nonzero.
 */
esp_err_t fleet_jitter_slot_for_mac(const uint8_t mac[6], uint32_t slot_count,
                                    uint32_t *slot_out);

/**
 * Read the Wi-Fi STA MAC and map it into [0, slot_count).
 *
 * Returns the esp_read_mac error unchanged. Logging and caller-specific
 * fallback policy remain the responsibility of the consumer.
 */
esp_err_t fleet_jitter_slot_for_sta_mac(uint32_t slot_count, uint32_t *slot_out);

#ifdef __cplusplus
}
#endif
