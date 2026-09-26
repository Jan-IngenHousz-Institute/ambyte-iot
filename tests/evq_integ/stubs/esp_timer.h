#pragma once
#include <stdint.h>
/* Virtual monotonic microseconds since (virtual) boot — rtos_shim.c. */
int64_t esp_timer_get_time(void);
