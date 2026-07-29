#include "ambyte_mqtt_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_client.h"

#define TAG                  "mqtt_client"

/* Reassembly caps for an inbound command. esp-mqtt delivers payloads larger than
 * CONFIG_MQTT_BUFFER_SIZE across several MQTT_EVENT_DATA fragments; we stitch them
 * here. Messages up to INBOUND_MSG_MAX use a static (BSS) buffer — no heap churn
 * for routine commands (ping, ota_update, lua_exec). Larger ones (an inline-Lua
 * script_update can be a full ~8 KiB main.lua) use a TRANSIENT heap buffer sized
 * to the message and freed right after dispatch — no permanent BSS cost on the
 * heap-tight board. Anything over INBOUND_MSG_LARGE_MAX (or unallocatable) is
 * dropped with a warning. */
#define INBOUND_MSG_MAX        2048
#define INBOUND_MSG_LARGE_MAX  16384
#define INBOUND_TOPIC_MAX      192

/* Keep this at or above SYNC_CONN_ERROR_THRESHOLD in sync_runner. A cap below
 * that threshold silently makes the rolling-window watchdog impossible to
 * satisfy; 64 leaves tuning headroom while this hot-path state stays tiny. */
#define ERROR_DISC_HISTORY_CAP 64

static esp_mqtt_client_handle_t s_client    = NULL;
static volatile bool            s_connected = false;
static bool                     s_started   = false;

/* Error-disconnect history. MQTT callbacks write it while the watchdog reads it
 * from another task/core, so protect the 64-bit timestamps with a spinlock. A
 * failed connection can emit ERROR followed by DISCONNECTED; the per-attempt
 * flag collapses those into one failure episode instead of double-counting it. */
static int64_t                  s_error_disc_us[ERROR_DISC_HISTORY_CAP];
static uint32_t                 s_error_disc_head;
static uint32_t                 s_error_disc_used;
static bool                     s_connect_failure_counted;
static portMUX_TYPE             s_error_disc_mux = portMUX_INITIALIZER_UNLOCKED;

/* Boot-scoped connection telemetry shares the disconnect-history lock so the
 * watchdog can read a coherent snapshot from the other core. The controlled
 * reason strings are deliberately short enough to copy while holding it. */
#define LAST_DISC_REASON_CAP 24
static uint32_t                 s_successful_connects;
static int64_t                  s_connected_since_us = -1;
static int                      s_pending_error_type;
static char                     s_last_disc_reason[LAST_DISC_REASON_CAP];

/* Outbound ack callback */
static message_publish_ack_fn s_ack_handler = NULL;
static void                  *s_ack_ctx     = NULL;

/* Transport-connect callback */
static message_connect_fn  s_connect_handler = NULL;
static void               *s_connect_ctx     = NULL;

/* Transport-disconnect callback (MQTT-level; fires even when Wi-Fi stays up) */
static message_disconnect_fn  s_disconnect_handler = NULL;
static void                  *s_disconnect_ctx     = NULL;

/* Inbound: subscribe target + received-message callback + reassembly state */
static char                 s_command_topic[INBOUND_TOPIC_MAX] = {0};
static message_received_fn  s_msg_handler = NULL;
static void                *s_msg_ctx     = NULL;
static char                 s_rx_buf[INBOUND_MSG_MAX + 1];
static char                *s_rx_large    = NULL;   /* transient, only while a >2 KiB message is in flight */
static char                 s_rx_topic[INBOUND_TOPIC_MAX];
static int                  s_rx_len      = 0;
static bool                 s_rx_overflow = false;

static void rx_large_free(void)
{
    free(s_rx_large);
    s_rx_large = NULL;
}

/* ── port implementations ──────────────────────────────────────────── */

static esp_err_t mqtt_publish_impl(const char *topic, const char *payload,
                                   size_t len, int *out_msg_id)
{
    if (s_client == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, (int)len, 1, 0);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    if (out_msg_id != NULL) {
        *out_msg_id = msg_id;
    }
    return ESP_OK;
}

/* Store the complete QoS-1 packet in ESP-MQTT's outbox, but leave every socket
 * write to the mqtt task.  Unlike esp_mqtt_client_publish(), this returns the
 * packet id without synchronously putting the packet on the wire.  The windowed
 * drain uses that separation to finish its msg_id latch before a PUBACK can be
 * consumed; other command/status publishers keep the synchronous port above. */
static esp_err_t mqtt_enqueue_impl(const char *topic, const char *payload,
                                   size_t len, int *out_msg_id)
{
    if (s_client == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_enqueue(s_client, topic, payload, (int)len,
                                         1, 0, true);
    if (msg_id < 0) {
        return msg_id == -2 ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    if (out_msg_id != NULL) {
        *out_msg_id = msg_id;
    }
    return ESP_OK;
}

static bool mqtt_is_connected_impl(void)
{
    return s_connected;
}

/* Caller holds s_error_disc_mux. Keeping the state transition and timestamp
 * insertion under one lock prevents ERROR/DISCONNECTED callbacks on another
 * core from observing a half-updated connection attempt. */
static void note_error_disconnect_locked(int64_t now_us)
{
    s_error_disc_us[s_error_disc_head] = now_us;
    s_error_disc_head = (s_error_disc_head + 1U) % ERROR_DISC_HISTORY_CAP;
    if (s_error_disc_used < ERROR_DISC_HISTORY_CAP) s_error_disc_used++;
}

static uint32_t mqtt_error_disconnect_count_impl(uint32_t window_s)
{
    int64_t now_us = esp_timer_get_time();
    int64_t window_us = (int64_t)window_s * 1000000LL;
    uint32_t count = 0;

    portENTER_CRITICAL(&s_error_disc_mux);
    for (uint32_t i = 0; i < s_error_disc_used; i++) {
        int64_t age_us = now_us - s_error_disc_us[i];
        if (age_us >= 0 && age_us <= window_us) count++;
    }
    portEXIT_CRITICAL(&s_error_disc_mux);
    return count;
}

static void mqtt_connection_stats_impl(uint32_t *successful_connects,
                                       int64_t *connection_age_s,
                                       char *last_disconnect_reason,
                                       size_t reason_cap)
{
    int64_t connected_since_us;
    bool connected;

    portENTER_CRITICAL(&s_error_disc_mux);
    connected = s_connected;
    connected_since_us = s_connected_since_us;
    if (successful_connects != NULL) *successful_connects = s_successful_connects;
    if (last_disconnect_reason != NULL && reason_cap > 0) {
        size_t copy_len = strnlen(s_last_disc_reason, sizeof s_last_disc_reason);
        if (copy_len >= reason_cap) copy_len = reason_cap - 1;
        memcpy(last_disconnect_reason, s_last_disc_reason, copy_len);
        last_disconnect_reason[copy_len] = '\0';
    }
    portEXIT_CRITICAL(&s_error_disc_mux);

    if (connection_age_s != NULL) {
        int64_t age_us = connected && connected_since_us >= 0
            ? esp_timer_get_time() - connected_since_us : -1;
        *connection_age_s = age_us >= 0 ? age_us / 1000000LL : -1;
    }
}

void mqtt_client_note_wifi_disconnect(uint8_t reason)
{
    char classification[LAST_DISC_REASON_CAP];
    snprintf(classification, sizeof classification, "wifi:%u", (unsigned)reason);

    portENTER_CRITICAL(&s_error_disc_mux);
    strncpy(s_last_disc_reason, classification, sizeof(s_last_disc_reason) - 1);
    s_last_disc_reason[sizeof(s_last_disc_reason) - 1] = '\0';
    s_pending_error_type = -1; /* preserve Wi-Fi cause if stop emits DISCONNECTED */
    s_connected = false;
    s_connected_since_us = -1;
    portEXIT_CRITICAL(&s_error_disc_mux);
}

static esp_err_t mqtt_set_ack_handler_impl(message_publish_ack_fn handler, void *ctx)
{
    s_ack_handler = handler;
    s_ack_ctx     = ctx;
    return ESP_OK;
}

static esp_err_t mqtt_set_received_handler_impl(message_received_fn handler, void *ctx)
{
    s_msg_handler = handler;
    s_msg_ctx     = ctx;
    return ESP_OK;
}

static esp_err_t mqtt_set_connect_handler_impl(message_connect_fn handler, void *ctx)
{
    s_connect_handler = handler;
    s_connect_ctx     = ctx;
    return ESP_OK;
}

static esp_err_t mqtt_set_disconnect_handler_impl(message_disconnect_fn handler, void *ctx)
{
    s_disconnect_handler = handler;
    s_disconnect_ctx     = ctx;
    return ESP_OK;
}

/* Stitch one MQTT_EVENT_DATA fragment into s_rx_buf and, on the final fragment,
 * deliver the whole NUL-terminated payload to the registered handler. esp-mqtt
 * provides the topic only on the first fragment (current_data_offset == 0). */
static void handle_inbound_data(esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0) {
        s_rx_len      = 0;
        s_rx_overflow = false;
        rx_large_free();   /* drop any half-assembled previous message */
        if (event->total_data_len > INBOUND_MSG_MAX) {
            if (event->total_data_len <= INBOUND_MSG_LARGE_MAX) {
                s_rx_large = malloc((size_t)event->total_data_len + 1);
            }
            /* > large cap, or the transient alloc failed → drop when complete */
            s_rx_overflow = (s_rx_large == NULL);
        }
        size_t tl = (event->topic_len > 0 && (size_t)event->topic_len < sizeof(s_rx_topic))
                        ? (size_t)event->topic_len : 0;
        if (tl > 0) {
            memcpy(s_rx_topic, event->topic, tl);
        }
        s_rx_topic[tl] = '\0';
    }

    char *dst = (s_rx_large != NULL) ? s_rx_large : s_rx_buf;
    int   cap = (s_rx_large != NULL) ? event->total_data_len : INBOUND_MSG_MAX;
    if (!s_rx_overflow && event->data_len > 0) {
        if (event->current_data_offset + event->data_len <= cap) {
            memcpy(dst + event->current_data_offset, event->data, event->data_len);
            s_rx_len = event->current_data_offset + event->data_len;
        } else {
            s_rx_overflow = true;
        }
    }

    /* Final fragment? */
    if (event->current_data_offset + event->data_len >= event->total_data_len) {
        if (s_rx_overflow) {
            ESP_LOGW(TAG, "inbound message %d B > cap %d (or no heap) — dropped (topic=%s)",
                     event->total_data_len, INBOUND_MSG_LARGE_MAX, s_rx_topic);
            rx_large_free();
            return;
        }
        dst[s_rx_len] = '\0';
        ESP_LOGI(TAG, "inbound %d B on %s", s_rx_len, s_rx_topic);
        if (s_msg_handler != NULL) {
            s_msg_handler(s_rx_topic, dst, (size_t)s_rx_len, s_msg_ctx);
        }
        rx_large_free();   /* transient buffer lives only until dispatch returns */
    }
}

/* ── MQTT event handler ────────────────────────────────────────────── */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        portENTER_CRITICAL(&s_error_disc_mux);
        s_connected = true;
        s_connected_since_us = esp_timer_get_time();
        s_successful_connects++;
        s_pending_error_type = 0;
        s_connect_failure_counted = false;
        portEXIT_CRITICAL(&s_error_disc_mux);
        ESP_LOGI(TAG, "MQTT connected");
        /* (Re)subscribe to the command topic on every connect — a clean session
         * drops subscriptions, so this must run on each reconnect, not once. */
        if (s_command_topic[0] != '\0') {
            int sub_id = esp_mqtt_client_subscribe(s_client, s_command_topic, 1);
            ESP_LOGI(TAG, "subscribing to %s (msg_id=%d)", s_command_topic, sub_id);
        }
        if (s_connect_handler != NULL) {
            s_connect_handler(s_connect_ctx);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        handle_inbound_data(event);
        break;

    case MQTT_EVENT_DISCONNECTED:
        /* Established-session drops include stalled/write-timeout failures. If
         * a failed CONNECT already emitted TCP_TRANSPORT ERROR, do not count the
         * immediately-following DISCONNECTED a second time. */
        portENTER_CRITICAL(&s_error_disc_mux);
        if (s_connected || !s_connect_failure_counted) {
            note_error_disconnect_locked(esp_timer_get_time());
        }
        if (s_pending_error_type == 0) {
            memcpy(s_last_disc_reason, "mqtt:disconnect", sizeof("mqtt:disconnect"));
        }
        s_connected = false;
        s_connected_since_us = -1;
        s_pending_error_type = 0;
        s_connect_failure_counted = false;
        portEXIT_CRITICAL(&s_error_disc_mux);
        rx_large_free();   /* don't hold a half-assembled message across the gap */
        ESP_LOGW(TAG, "MQTT disconnected");
        /* Clear any in-flight publish slot now, even though Wi-Fi is still up:
         * the message this session was awaiting a PUBACK for is gone, so the
         * app must revert it to PENDING or the drain wedges on reconnect. */
        if (s_disconnect_handler != NULL) {
            s_disconnect_handler(s_disconnect_ctx);
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "MQTT publish ack msg_id=%d", event->msg_id);
        if (s_ack_handler != NULL) {
            s_ack_handler(event->msg_id, ESP_OK, s_ack_ctx);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type=%d", event->error_handle->error_type);
        char classification[LAST_DISC_REASON_CAP];
        snprintf(classification, sizeof classification, "mqtt:%d",
                 event->error_handle->error_type);
        /* While disconnected, a TCP transport error is a failed broker/TLS
         * connection attempt. Established-session transport failures are counted
         * by the DISCONNECTED event that follows. */
        portENTER_CRITICAL(&s_error_disc_mux);
        strncpy(s_last_disc_reason, classification, sizeof(s_last_disc_reason) - 1);
        s_last_disc_reason[sizeof(s_last_disc_reason) - 1] = '\0';
        s_pending_error_type = event->error_handle->error_type;
        if (!s_connected && !s_connect_failure_counted &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            note_error_disconnect_locked(esp_timer_get_time());
            s_connect_failure_counted = true;
        }
        portEXIT_CRITICAL(&s_error_disc_mux);
        if (s_ack_handler != NULL && event->msg_id > 0) {
            s_ack_handler(event->msg_id, ESP_FAIL, s_ack_ctx);
        }
        break;

    default:
        break;
    }
}

/* ── public API ────────────────────────────────────────────────────── */

esp_err_t mqtt_client_init(const mqtt_client_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool tls_ok = cfg->ca_cert_pem     != NULL && cfg->ca_cert_pem[0]     != '\0' &&
                  cfg->device_cert_pem != NULL && cfg->device_cert_pem[0] != '\0' &&
                  cfg->device_key_pem  != NULL && cfg->device_key_pem[0]  != '\0';

    /* Retain the command topic for (re)subscription on each connect. */
    if (cfg->command_topic != NULL && cfg->command_topic[0] != '\0') {
        strncpy(s_command_topic, cfg->command_topic, sizeof(s_command_topic) - 1);
        s_command_topic[sizeof(s_command_topic) - 1] = '\0';
    } else {
        s_command_topic[0] = '\0';
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri              = cfg->broker_uri,
            .verification.certificate = tls_ok ? cfg->ca_cert_pem : NULL,
        },
        .credentials = {
            .client_id = cfg->client_id,
            .authentication = {
                .certificate = tls_ok ? cfg->device_cert_pem : NULL,
                .key         = tls_ok ? cfg->device_key_pem  : NULL,
            },
        },
        .session = {
            .protocol_ver               = MQTT_PROTOCOL_V_5,
            .keepalive                  = 300,     /* s; matched to the STATUS heartbeat. Cuts idle
                                                    * PINGREQ from the 120 s default (~720->~288/day);
                                                    * within AWS IoT's 30-1200 s (broker drops after
                                                    * 1.5x keepalive of true silence). */
            /* The 1 s default re-sends QoS-1 messages throughout a degraded
             * PUBACK path, multiplying duplicates ~5-6x and consuming the
             * bandwidth needed for the original publish to finish. */
            .message_retransmit_timeout = 10000,
        },
        /* Keep esp-mqtt's default auto-reconnect. It has no per-attempt veto or
         * cancellable BEFORE_CONNECT hook; gating TLS handshakes on measurement
         * activity would require disabling it and adding a separate retry
         * scheduler. That machinery is intentionally outside the lightweight
         * publish-gate split, so reconnect timing remains unchanged. */
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "MQTT client initialised (TLS=%s)", tls_ok ? "yes" : "no");
    return ESP_OK;
}

void mqtt_client_start(void)
{
    if (s_client != NULL && !s_started) {
        esp_mqtt_client_start(s_client);
        s_started = true;
    }
}

void mqtt_client_stop(void)
{
    /* Idempotent: silently skip if we never started. Prevents the esp-mqtt
     * "Client asked to stop, but was not started" warning on every Wi-Fi
     * disconnect attempt when the broker never came up. */
    if (s_client != NULL && s_started) {
        portENTER_CRITICAL(&s_error_disc_mux);
        s_connected = false;
        s_connected_since_us = -1;
        portEXIT_CRITICAL(&s_error_disc_mux);
        s_started   = false;
        esp_mqtt_client_stop(s_client);
    }
}

bool mqtt_client_is_running(void)
{
    return s_started;
}

message_publish_fn mqtt_client_get_publish_fn(void)
{
    return mqtt_publish_impl;
}

message_enqueue_fn mqtt_client_get_enqueue_fn(void)
{
    return mqtt_enqueue_impl;
}

message_is_connected_fn mqtt_client_get_is_connected_fn(void)
{
    return mqtt_is_connected_impl;
}

message_error_disconnect_count_fn mqtt_client_get_error_disconnect_count_fn(void)
{
    return mqtt_error_disconnect_count_impl;
}

message_connection_stats_fn mqtt_client_get_connection_stats_fn(void)
{
    return mqtt_connection_stats_impl;
}

message_set_connect_handler_fn mqtt_client_get_set_connect_handler_fn(void)
{
    return mqtt_set_connect_handler_impl;
}

message_set_publish_ack_handler_fn mqtt_client_get_set_ack_handler_fn(void)
{
    return mqtt_set_ack_handler_impl;
}

message_set_received_handler_fn mqtt_client_get_set_received_handler_fn(void)
{
    return mqtt_set_received_handler_impl;
}

message_set_disconnect_handler_fn   mqtt_client_get_set_disconnect_handler_fn(void)
{
    return mqtt_set_disconnect_handler_impl;
}
