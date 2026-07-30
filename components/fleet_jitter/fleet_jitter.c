#include "fleet_jitter.h"

#include <stddef.h>

#include "esp_mac.h"

#define FLEET_JITTER_MAC_BYTES        6U
#define FLEET_JITTER_FNV_OFFSET_BASIS 2166136261U
#define FLEET_JITTER_FNV_PRIME        16777619U

esp_err_t fleet_jitter_slot_for_mac(const uint8_t mac[6], uint32_t slot_count,
                                    uint32_t *slot_out)
{
    if (mac == NULL || slot_out == NULL || slot_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t hash = FLEET_JITTER_FNV_OFFSET_BASIS;
    for (size_t i = 0; i < FLEET_JITTER_MAC_BYTES; i++) {
        hash ^= mac[i];
        hash *= FLEET_JITTER_FNV_PRIME;
    }

    *slot_out = hash % slot_count;
    return ESP_OK;
}

esp_err_t fleet_jitter_slot_for_sta_mac(uint32_t slot_count, uint32_t *slot_out)
{
    if (slot_out == NULL || slot_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[FLEET_JITTER_MAC_BYTES] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }

    return fleet_jitter_slot_for_mac(mac, slot_count, slot_out);
}
