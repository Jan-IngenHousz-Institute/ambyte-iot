#pragma once

#include "esp_err.h"
#include "messaging_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Inbound MQTT command router (docs/ota-update-plan.md, Stage 2).
 *
 * Parses a JSON command delivered on the device's command topic and dispatches on
 * its "type" field. The shared inbound channel for both the OTA trigger (Stage 3)
 * and the Lua script push (Stage 4). Register command_router_get_received_fn() with
 * the transport (mqtt_client_get_set_received_handler_fn) after init.
 *
 * Command shape:
 *   { "type": "ping" | "ota_update" | "script_update", "id": "<unique>", ... }
 * State-changing commands carry a unique `id`; the router persists the last applied
 * id in NVS and no-ops a repeat (idempotency — safe with a retained trigger).
 */

typedef struct {
    message_publish_fn  publish;          /* mqtt_client_get_publish_fn() — for replies/acks */
    const char         *status_topic;     /* topic for replies, e.g. "<topic_root>/status" */
    const char         *device_id;        /* included in reply payloads */
    const char         *firmware_version; /* included in reply payloads */
} command_router_config_t;

esp_err_t command_router_init(const command_router_config_t *cfg);

/* The message_received_fn to register with the transport. NULL until init runs. */
message_received_fn command_router_get_received_fn(void);

#ifdef __cplusplus
}
#endif
