#pragma once

#include "messaging_port.h"
#include "sensing_port.h"

typedef struct {
    message_publish_fn publish;
    message_is_connected_fn mqtt_connected;
    bool (*wifi_connected)(void);
    bool (*publish_allowed)(void); /* optional transient sensor hold; never a power gate */
    power_read_fn read_power; /* optional; failed/absent read reports power:null */
    bool (*sd_ready)(void);   /* optional; no SD access, cached mount state only */
    const char *status_topic;
    const char *device_id;
    const char *firmware_version;
} status_heartbeat_config_t;

/* Start once after MQTT and power-driver initialization. Copies the config;
 * string storage must outlive the task. One small QoS-1 status-topic message
 * at a MAC-staggered point within 15 minutes of connection, then every 15
 * minutes of uptime. Brief sensor holds defer a due report (checked every 1 s).
 * Retries failed
 * submissions / overdue offline reports every 30 s, without offline buffering.
 *
 * Deliberately independent of external power, SD/internal storage, RTC, schedule
 * and measurement-backlog gates. Does not acquire a measurement ID, touch a
 * cursor or wake sensors. MQTT transport remains responsible for QoS-1 delivery;
 * submission success is not proof of receipt. A powered, connected device is
 * required — this cannot report through a shutdown or network outage. */
esp_err_t status_heartbeat_start(const status_heartbeat_config_t *config);
