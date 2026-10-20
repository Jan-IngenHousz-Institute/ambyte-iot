#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
typedef enum { USJ_SELECT_WRITE_NOTIF, USJ_SELECT_READ_NOTIF } usj_select_notif_t;
typedef void (*usj_select_notif_callback_t)(usj_select_notif_t,BaseType_t *);
