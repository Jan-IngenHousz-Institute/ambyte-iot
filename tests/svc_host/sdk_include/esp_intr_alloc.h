#pragma once
#include "esp_err.h"
#define ETS_USB_SERIAL_JTAG_INTR_SOURCE 1
typedef void *intr_handle_t;
esp_err_t esp_intr_alloc(int,int,void (*)(void *),void *,intr_handle_t *);
void esp_intr_free(intr_handle_t);
