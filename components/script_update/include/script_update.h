#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "messaging_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Remote Lua control over MQTT (docs/device-script-delivery.md, Stage 4):
 *
 *  - script_update: replace /sdcard/main.lua with an inline-delivered script
 *    and restart the Lua runner. The script is syntax-checked BEFORE the SD is
 *    touched; the previous main.lua survives as main.lua.bak. Optional sha256
 *    integrity check. NVS id latch (success only) dedupes the retained topic.
 *  - lua_exec: run a short Lua snippet immediately in an ephemeral state
 *    (lua_runner_exec) and publish its result — remote-CLI parity.
 *
 * Both run on a lazy worker task (spawned on demand, exits when idle — zero
 * steady-state heap, same pattern as ambit_ota) so neither blocks the MQTT
 * event task.
 */

/* Suspend/resume the Lua measurement workload (app_workload_suspend/resume). The
 * `url` variant needs these: stopping Lua frees its 8 KB buffer and defragments
 * the heap so the download's TLS handshake gets its contiguous record buffer. */
typedef void (*script_workload_fn)(void);

typedef struct {
    message_publish_fn      publish;        /* status/result reports; NULL = log-only */
    message_is_connected_fn is_connected;
    const char             *status_topic;
    const char             *device_id;
    script_workload_fn      workload_suspend;  /* NULL disables the url variant */
    script_workload_fn      workload_resume;
    script_workload_fn      comms_suspend;      /* mqtt_client_stop — free MQTT's TLS heap for the download */
    script_workload_fn      comms_resume;       /* mqtt_client_start — reconnect before reporting */
} script_update_config_t;

/* Prepare the worker plumbing (idempotent; the task itself spawns on demand). */
esp_err_t script_update_init(const script_update_config_t *cfg);

/* Queue a main.lua replacement. `script` is copied (caller's buffer may die);
 * `checksum` (optional, NULL ok) is the lowercase/uppercase hex SHA-256 of the
 * script; `id` dedupes a retained message (latched on success only; NULL = no
 * dedupe). `reboot` = true (the default) restarts the whole device after a
 * successful swap so the new script runs from a fresh boot; false keeps the old
 * in-place behaviour (stop + swap + restart just the Lua runner). Either way the
 * id is latched on success FIRST, so a retained trigger can't loop the reboot.
 * Reports {"type":"script_status",...,"state":"applied"|"failed"}.
 * ESP_ERR_INVALID_STATE before init; ESP_ERR_NO_MEM if busy or out of memory. */
esp_err_t script_update_request(const char *script, const char *checksum, const char *id,
                                bool reboot);

/* Queue a main.lua replacement fetched from `url` (HTTPS, streamed to SD in 4 KB
 * chunks). Unlike the inline variant this needs NO large contiguous MQTT/TLS
 * buffer, so it lands reliably on a fragmented heap — the worker stops Lua first
 * to defragment, downloads, verifies (`checksum` = sha256 hex of the file, optional)
 * and syntax-checks, then swaps + reboots/restarts. Requires workload_suspend/
 * resume in the config (else ESP_ERR_INVALID_STATE-style "unavailable" report).
 * `id`/`reboot` semantics match script_update_request. */
esp_err_t script_update_url_request(const char *url, const char *checksum, const char *id,
                                    bool reboot);

/* Queue a snippet for immediate execution (lua_runner_exec, 120 s budget).
 * Reports {"type":"lua_exec_result",...,"ok":…,"result":"…"}. */
esp_err_t script_update_exec_request(const char *code, const char *id);

#ifdef __cplusplus
}
#endif
