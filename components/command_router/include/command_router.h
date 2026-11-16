#pragma once

#include <stdbool.h>
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
 * and schedule installation. Register command_router_get_received_fn() with
 * the transport (mqtt_client_get_set_received_handler_fn) after init.
 *
 * Command shape:
 *   { "type": "ping" | "ota_update" | "script_update", "id": "<unique>", ... }
 * script_update release messages may add `script_version` and `built_against_fw`;
 * older command payloads remain valid.
 * Read-only diagnostics: `ping` → `pong`; `evlog_inventory` → one
 * `evlog_inventory` report describing the SD archive (arc-*.log) with optional
 * `from_id`/`to_id`/`from_ms`/`to_ms` window and `list` (per-file rows, default
 * true). It never touches the event store, the cursor or the card contents.
 * `get_vin_limit` / `set_vin_limit` (integer `mv`) read/write the live charger
 * input limit on the resident worker; replies carry verified REG01 readback.
 * VIN-limit commands require a nonempty id of at most 64 bytes, use no NVS
 * dedupe/override, and repeated sets write the same desired register value.
 * `get_iin_limit` / `set_iin_limit` (integer `ma`) do the same for the charger
 * input current limit REGISTER (REG00[5:0]); the set preserves REG00 bits 7:6
 * (HIZ, EN_ILIM) and replies carry verified REG00 plus `en_ilim`/`hiz`. Same
 * id rules and single-flight `busy` as VIN-limit, and independent of it.
 * `charger_redetect` (`method` bg_en|dpdm, default bg_en) pulses the charger's
 * input detection on the worker and replies with before/after register scans;
 * `charger_redetect_auto` (`enabled` bool, omitted = query) flips the RAM-only
 * automatic re-detection. Same id rules as VIN-limit, no NVS.
 * Update components own their individual idempotency policies.
 */

typedef struct {
    message_publish_fn  publish;          /* mqtt_client_get_publish_fn() — for replies/acks */
    const char         *status_topic;     /* topic for replies, e.g. "<topic_root>/status" */
    const char         *device_id;        /* included in reply payloads */
    const char         *firmware_version; /* included in reply payloads */
    bool              (*submit)(void (*run)(void *), void *arg);
    bool              (*maintenance_begin)(void);
    void              (*maintenance_end)(void);
} command_router_config_t;

esp_err_t command_router_init(const command_router_config_t *cfg);

/* The message_received_fn to register with the transport. NULL until init runs. */
message_received_fn command_router_get_received_fn(void);

#ifdef __cplusplus
}
#endif
