/* Host stub: route logs through the harness so tests can assert on them, and
 * so -Wformat checks every production format string against its arguments. */
#ifndef WM_STUB_ESP_LOG_H
#define WM_STUB_ESP_LOG_H
void wm_stub_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, fmt, ...) wm_stub_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) wm_stub_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) wm_stub_log('I', tag, fmt, ##__VA_ARGS__)
#endif
