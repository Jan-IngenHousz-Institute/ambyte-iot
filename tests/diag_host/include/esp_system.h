/* Host stand-in for the ONE declaration diag_reset.c needs: ESP-IDF 5.5's
 * esp_reset_reason_t, same order and values as
 * framework-espidf@3.50500.0/components/esp_system/include/esp_system.h
 * (tests/test_diag.py asserts that this copy matches the pinned SDK). */
#pragma once

typedef enum {
    ESP_RST_UNKNOWN,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
    ESP_RST_USB,
    ESP_RST_JTAG,
    ESP_RST_EFUSE,
    ESP_RST_PWR_GLITCH,
    ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;
