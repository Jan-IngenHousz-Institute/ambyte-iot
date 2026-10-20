#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool (*submit)(void (*run)(void *), void *arg);
    bool (*maintenance_begin)(void);
    void (*maintenance_end)(void);
    bool (*boot_ready)(void);
    uint32_t (*worker_stack)(void);
} usb_service_config_t;

/* Init after littlefs and boot Wi-Fi config, before CLI starts. This captures
 * the boot config fingerprint before any legacy console mutation can occur. */
esp_err_t usb_service_init(const usb_service_config_t *config);
void usb_service_boot_ready(void);
bool usb_service_wifi_busy(void);
int usb_service_command(int argc, char **argv);
