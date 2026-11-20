/* Host stand-in: the harness supplies esp_random (deterministic). */
#pragma once
#include <stdint.h>
uint32_t esp_random(void);
