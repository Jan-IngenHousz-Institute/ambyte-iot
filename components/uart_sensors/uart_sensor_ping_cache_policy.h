#ifndef UART_SENSOR_PING_CACHE_POLICY_H
#define UART_SENSOR_PING_CACHE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* A successful reply is authoritative even while an AMBIT bank is rebooting.
 * A failure is not: the application may simply not have reached its UART router
 * yet. Keeping this tiny decision pure lets the reset/cache boundary be checked
 * on the host without mocking the ESP-IDF UART and GPIO drivers. */
static inline bool uart_sensor_ping_result_cacheable(
    bool connected,
    int64_t observed_at_us,
    int64_t cache_failures_after_us)
{
    return connected || observed_at_us >= cache_failures_after_us;
}

#endif
