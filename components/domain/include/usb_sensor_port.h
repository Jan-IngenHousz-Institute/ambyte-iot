#ifndef AMBYTE_USB_SENSOR_PORT_H
#define AMBYTE_USB_SENSOR_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB CDC-ACM sensors live behind ONE powered hub; a logical channel maps to a
 * fixed physical hub downstream port (see usb_sensors.c s_port_map). All units
 * are otherwise indistinguishable (shared VID/PID/serial), so identity is
 * positional. */
#define USB_SENSOR_NUM_CHANNELS  4

typedef enum {
    USB_SENSOR_DISCONNECTED = 0,
    USB_SENSOR_CONNECTED    = 1,
    USB_SENSOR_BUSY         = 2,
} usb_sensor_state_t;

/* usb_sensor_text_query_fn — send an ASCII command line and read one response
 * line. Mirrors uart_sensor_text_query_fn so it slots into the same Lua/command
 * surface. Sends `cmd` followed by `terminator`, then accumulates incoming bytes
 * until `terminator` is received or `timeout_ms` elapses. The terminator is
 * stripped from `out_resp`. On timeout *resp_len is 0, out_resp[0]='\0', returns
 * ESP_ERR_TIMEOUT. Returns ESP_ERR_NOT_FOUND when no device is on that channel.
 *
 *   channel    : 0..USB_SENSOR_NUM_CHANNELS-1 (physical hub port)
 *   cmd        : NUL-terminated ASCII command (no terminator appended yet)
 *   terminator : line terminator for send framing + receive delimiter (e.g. "\n")
 *   out_resp   : caller buffer, NUL-terminated on return
 *   resp_cap   : sizeof(out_resp); response truncated at resp_cap-1
 *   resp_len   : OUT — bytes written to out_resp (excluding the NUL)
 *   timeout_ms : total wall-clock budget for send + read
 */
typedef esp_err_t (*usb_sensor_text_query_fn)(uint8_t channel,
                                              const char *cmd,
                                              const char *terminator,
                                              char       *out_resp,
                                              size_t      resp_cap,
                                              size_t     *resp_len,
                                              uint32_t    timeout_ms);

/* usb_sensor_ping_fn — report whether a device is currently bound to `channel`. */
typedef esp_err_t (*usb_sensor_ping_fn)(uint8_t channel, bool *connected);

/* usb_sensor_status_fn — current per-channel state. */
typedef esp_err_t (*usb_sensor_status_fn)(uint8_t channel, usb_sensor_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_USB_SENSOR_PORT_H */
