#ifndef WM_STUB_ESP_TIMER_H
#define WM_STUB_ESP_TIMER_H
#include <stdint.h>
#include "esp_err.h"
typedef struct wm_stub_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    const char *name;
} esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
int64_t esp_timer_get_time(void);   /* virtual clock in the harness */
#endif
