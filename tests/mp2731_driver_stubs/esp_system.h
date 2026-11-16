#pragma once
/* Same order as ESP-IDF 5.5 esp_system.h; each host test defines the function. */
typedef enum {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
    ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT,
} esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
