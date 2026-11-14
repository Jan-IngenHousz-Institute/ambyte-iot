#pragma once
#include <stdint.h>
/* A production self-reboot is a test failure in this harness (esp_stubs.c). */
void esp_restart(void) __attribute__((noreturn));
uint32_t esp_get_free_heap_size(void);

/* esp_reset_reason_t as in the pinned ESP-IDF 5.5 (diag_reset.h needs the
 * type; tests/test_diag.py pins the order against the SDK). */
typedef enum {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
    ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT,
    ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE, ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;
