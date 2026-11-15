/* Host stand-in: every log line reaches the driver, so the tests can count the
 * ESP_LOGE transition lines exactly (one per transition). printf-checked. */
#ifndef PP_HOST_ESP_LOG_H
#define PP_HOST_ESP_LOG_H
void host_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) host_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log('I', tag, __VA_ARGS__)
#endif
