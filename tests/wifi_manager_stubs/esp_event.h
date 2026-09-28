#ifndef WM_STUB_ESP_EVENT_H
#define WM_STUB_ESP_EVENT_H
#include <stdint.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base,
                                    int32_t id, void *data);
#define ESP_EVENT_ANY_ID -1
extern esp_event_base_t const WIFI_EVENT;
extern esp_event_base_t const IP_EVENT;
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
                                                esp_event_handler_instance_t instance);
#endif
