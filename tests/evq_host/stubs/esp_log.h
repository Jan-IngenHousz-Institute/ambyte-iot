#ifndef EVQ_HOST_ESP_LOG_H
#define EVQ_HOST_ESP_LOG_H
void evq_host_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) evq_host_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) evq_host_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) evq_host_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)0)
#endif
