/* Host stub: ESP_LOGx -> harness log (virtual timestamp + task), see rtos_shim.c. */
#pragma once
void h_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, fmt, ...) h_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) h_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) h_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) h_log('D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) h_log('V', tag, fmt, ##__VA_ARGS__)
