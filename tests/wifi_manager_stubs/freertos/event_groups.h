#ifndef WM_STUB_EVENT_GROUPS_H
#define WM_STUB_EVENT_GROUPS_H
#include "freertos/FreeRTOS.h"
typedef uint32_t EventBits_t;
typedef struct wm_stub_event_group *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupGetBits(EventGroupHandle_t g);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits,
                                BaseType_t clear_on_exit, BaseType_t wait_all,
                                TickType_t ticks);
#endif
