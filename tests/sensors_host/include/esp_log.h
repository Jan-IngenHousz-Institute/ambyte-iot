#pragma once
static inline void sensor_host_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
#define ESP_LOGI(...) sensor_host_log(__VA_ARGS__)
#define ESP_LOGW(...) sensor_host_log(__VA_ARGS__)
#define ESP_LOGE(...) sensor_host_log(__VA_ARGS__)
#define ESP_LOGD(...) sensor_host_log(__VA_ARGS__)
