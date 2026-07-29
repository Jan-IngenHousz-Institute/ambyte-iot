#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* out_msg_id receives the broker-assigned message ID (QoS 1); may be NULL for fire-and-forget */
typedef esp_err_t (*message_publish_fn)(const char *topic, const char *payload, size_t len,
                                        int *out_msg_id);
/* Queue a QoS-1 message into the transport outbox without performing the socket
 * write in the caller's task.  The returned msg_id is available before the MQTT
 * task performs network I/O, which lets a caller install its ACK-correlation
 * latch before a fast broker can answer. */
typedef esp_err_t (*message_enqueue_fn)(const char *topic, const char *payload, size_t len,
                                        int *out_msg_id);
typedef bool      (*message_is_connected_fn)(void);

/* Number of transport-error disconnect episodes seen inside the requested
 * rolling window. The transport owns the timestamp history so consumers do not
 * have to sample a cumulative counter at exactly the same cadence. */
typedef uint32_t  (*message_error_disconnect_count_fn)(uint32_t window_s);

/* Boot-scoped MQTT connection telemetry. successful_connects counts every
 * MQTT_EVENT_CONNECTED (including the first); connection_age_s is -1 while
 * disconnected. last_disconnect_reason is a short transport-owned
 * classification such as "mqtt:1" or "wifi:201". Any out-param may be NULL. */
typedef void      (*message_connection_stats_fn)(uint32_t *successful_connects,
                                                  int64_t *connection_age_s,
                                                  char *last_disconnect_reason,
                                                  size_t reason_cap);

/* Callback delivered when the transport establishes its MQTT connection.
 * Runs in the transport's task context, so handlers must stay lightweight. */
typedef void      (*message_connect_fn)(void *ctx);
typedef esp_err_t (*message_set_connect_handler_fn)(message_connect_fn handler,
                                                     void *ctx);

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
 * the app revert every unacknowledged publish-window slot so a reconnect does
 * not wedge behind messages that will never be ACKed. Runs in the transport's
 * task context. */
typedef void      (*message_disconnect_fn)(void *ctx);
typedef esp_err_t (*message_set_disconnect_handler_fn)(message_disconnect_fn handler,
                                                       void *ctx);

#ifdef __cplusplus
}
#endif
