#pragma once
#define ESP_RETURN_ON_ERROR(call, ...) do { esp_err_t e_ = (call); if (e_ != ESP_OK) return e_; } while (0)
