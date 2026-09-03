#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "uart_sensor_ping_cache_policy.h"

static int failures;

static void expect(const char *name, bool actual, bool expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got %s, want %s\n", name,
                actual ? "true" : "false", expected ? "true" : "false");
        failures++;
    }
}

int main(void)
{
    const int64_t cache_failures_after_us = 5000000;

    expect("success during reboot grace remains cacheable",
           uart_sensor_ping_result_cacheable(true, 1000000,
                                             cache_failures_after_us),
           true);
    expect("failure during reboot grace is retried",
           uart_sensor_ping_result_cacheable(false, 4999999,
                                             cache_failures_after_us),
           false);
    expect("failure at reboot grace boundary is cacheable",
           uart_sensor_ping_result_cacheable(false, 5000000,
                                             cache_failures_after_us),
           true);
    expect("ordinary later failure is cacheable",
           uart_sensor_ping_result_cacheable(false, 7000000,
                                             cache_failures_after_us),
           true);

    return failures == 0 ? 0 : 1;
}
