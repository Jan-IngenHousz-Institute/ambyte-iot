#ifndef SD_SHIM_ESP_LOG_H
#define SD_SHIM_ESP_LOG_H
/* Host log: routes through the installed vprintf sink exactly like IDF's
 * esp_log_write, so the production sd_logger hook (and its recursion guard)
 * sees real "W (t) tag: msg\n" lines. */
#include <stdarg.h>
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
void host_log_write(const char *fmt, ...);
#define ESP_LOGE(tag, format, ...) host_log_write("E (0) %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) host_log_write("W (0) %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) host_log_write("I (0) %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) ((void)0)
#endif
