#include "ambyte_mqtt_client.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
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

static esp_mqtt_client_handle_t s_client    = NULL;
static volatile bool            s_connected = false;
static bool                     s_started   = false;

/* Outbound ack callback */
static message_publish_ack_fn s_ack_handler = NULL;
static void                  *s_ack_ctx     = NULL;

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

static bool mqtt_is_connected_impl(void)
{
    return s_connected;
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
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        /* (Re)subscribe to the command topic on every connect — a clean session
         * drops subscriptions, so this must run on each reconnect, not once. */
        if (s_command_topic[0] != '\0') {
            int sub_id = esp_mqtt_client_subscribe(s_client, s_command_topic, 1);
            ESP_LOGI(TAG, "subscribing to %s (msg_id=%d)", s_command_topic, sub_id);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        handle_inbound_data(event);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
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
            .protocol_ver = MQTT_PROTOCOL_V_5,
            .keepalive    = 300,   /* s; matched to the STATUS heartbeat. Cuts idle
                                    * PINGREQ from the 120 s default (~720->~288/day);
                                    * within AWS IoT's 30-1200 s (broker drops after
                                    * 1.5x keepalive of true silence). */
        },
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
        s_connected = false;
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

message_is_connected_fn mqtt_client_get_is_connected_fn(void)
{
    return mqtt_is_connected_impl;
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
