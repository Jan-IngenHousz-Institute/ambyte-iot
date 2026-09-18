#pragma once

#include "messaging_port.h"

typedef struct {
    esp_err_t (*publish_snapshot)(void);
    message_is_connected_fn mqtt_connected;
    bool (*wifi_connected)(void);
    bool (*publish_allowed)(void); /* transient sensor hold, never the power gate */
} status_heartbeat_config_t;

/* One fresh canonical telemetry envelope on the normal ingestion topic every
 * 15 minutes while connected. MAC-staggered uptime grid, overdue across flaps,
 * 30-second submission retry and 1-second sensor-hold polling. The callback
 * owns telemetry construction and unique-ID allocation, without draining or
 * advancing the measurement FIFO. QoS1 submission is not ingestion proof. */
esp_err_t status_heartbeat_start(const status_heartbeat_config_t *config);
