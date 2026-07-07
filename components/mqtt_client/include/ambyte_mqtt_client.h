#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "messaging_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *broker_uri;
    const char *client_id;
    /* TLS credentials — all three must be non-NULL and non-empty to enable TLS.
     * Pass NULL (or empty strings) to connect without TLS. */
    const char *ca_cert_pem;
    const char *device_cert_pem;
    const char *device_key_pem;
    /* Inbound command topic to (re)subscribe to on every connect, QoS 1. NULL or
     * empty = no subscription (publish-only, the prior behaviour). The AWS IoT
     * policy must grant iot:Subscribe + iot:Receive on this topic. */
    const char *command_topic;
} mqtt_client_config_t;

esp_err_t mqtt_client_init(const mqtt_client_config_t *cfg);
void      mqtt_client_start(void);
void      mqtt_client_stop(void);
bool      mqtt_client_is_running(void);   /* true once start() has been called and stop() hasn't been */

message_publish_fn                  mqtt_client_get_publish_fn(void);
message_is_connected_fn             mqtt_client_get_is_connected_fn(void);
message_set_publish_ack_handler_fn  mqtt_client_get_set_ack_handler_fn(void);
message_set_received_handler_fn     mqtt_client_get_set_received_handler_fn(void);
message_set_disconnect_handler_fn   mqtt_client_get_set_disconnect_handler_fn(void);

#ifdef __cplusplus
}
#endif
