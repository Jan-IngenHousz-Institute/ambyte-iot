#ifndef PP_HOST_ESP_TIMER_H
#define PP_HOST_ESP_TIMER_H
#include <stdint.h>
int64_t esp_timer_get_time(void);   /* the driver's monotonic clock, µs */
#endif
