#pragma once
static inline void identity_host_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
#define ESP_LOGE(...) identity_host_log(__VA_ARGS__)
#define ESP_LOGW(...) identity_host_log(__VA_ARGS__)
#define ESP_LOGI(...) identity_host_log(__VA_ARGS__)
