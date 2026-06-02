#ifndef AMBYTE_USB_SENSORS_H
#define AMBYTE_USB_SENSORS_H

#include "esp_err.h"
#include "usb_sensor_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up USB-OTG host mode + the CDC-ACM driver and start tracking sensors
 * behind a single powered hub.
 *
 * Spawns the USB host-library daemon task and registers a CDC-ACM new-device
 * callback. Devices are bound to a logical channel by their physical hub
 * downstream port (see s_port_map in usb_sensors.c). Up to
 * USB_SENSOR_NUM_CHANNELS devices are tracked; connect/disconnect is handled at
 * runtime. Non-blocking: returns once the stack is installed; already-attached
 * devices appear shortly after via the callback.
 *
 * Must be called once from app_main, AFTER the console has been moved off the
 * USB-Serial-JTAG pads (GPIO19/20), which USB-OTG host mode claims.
 *
 * Returns ESP_OK, or ESP_ERR_INVALID_STATE if already initialised.
 */
esp_err_t usb_sensors_init(void);

/* Port-adapter getters — return function pointers wired into device_commands. */
usb_sensor_text_query_fn  usb_sensors_get_text_query_fn(void);
usb_sensor_ping_fn        usb_sensors_get_ping_fn(void);
usb_sensor_status_fn      usb_sensors_get_status_fn(void);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_USB_SENSORS_H */
