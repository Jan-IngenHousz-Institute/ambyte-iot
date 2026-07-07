#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* out_msg_id receives the broker-assigned message ID (QoS 1); may be NULL for fire-and-forget */
typedef esp_err_t (*message_publish_fn)(const char *topic, const char *payload, size_t len,
                                        int *out_msg_id);
typedef bool      (*message_is_connected_fn)(void);

/* Callback delivered when a QoS-1 publish is acknowledged (or fails) */
typedef void      (*message_publish_ack_fn)(int msg_id, esp_err_t status, void *ctx);
typedef esp_err_t (*message_set_publish_ack_handler_fn)(message_publish_ack_fn handler,
                                                         void *ctx);

/* Callback delivered when a complete message arrives on a subscribed topic.
 * `topic` and `payload` are NUL-terminated copies valid only for the duration of
 * the call (the transport reassembles multi-part MQTT_EVENT_DATA before calling).
 * `payload_len` excludes the terminating NUL. Runs in the transport's task
 * context — keep work light or hand off to another task. */
typedef void      (*message_received_fn)(const char *topic, const char *payload,
                                         size_t payload_len, void *ctx);
typedef esp_err_t (*message_set_received_handler_fn)(message_received_fn handler,
                                                     void *ctx);

/* Callback delivered when the transport loses its connection at the MQTT level
 * (i.e. MQTT_EVENT_DISCONNECTED — fires even when Wi-Fi stays associated). Lets
 * the app clear any in-flight publish slot so a reconnect doesn't wedge behind a
 * message that will never be ACKed. Runs in the transport's task context. */
typedef void      (*message_disconnect_fn)(void *ctx);
typedef esp_err_t (*message_set_disconnect_handler_fn)(message_disconnect_fn handler,
                                                       void *ctx);

#ifdef __cplusplus
}
#endif
