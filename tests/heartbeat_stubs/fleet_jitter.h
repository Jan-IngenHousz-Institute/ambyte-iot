#pragma once
#include <stdint.h>
#include "esp_err.h"
esp_err_t fleet_jitter_slot_for_sta_mac(uint32_t slots, uint32_t *out);
