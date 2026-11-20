#pragma once
/*
 * power_protect_command.h — the operator side of brownout protection
 * (power_protect.h, docs/power-protect.md), same resident-worker pattern as
 * set_timezone / log_read.
 *
 *   {"type":"power_protect","id":…,"action":"status"}
 *   {"type":"power_protect","id":…,"action":"clear"}
 *   {"type":"power_protect","id":…,"action":"force","state":"no_flash"|"hold"}
 *     → {"type":"power_protect_result","id","ok","action","detail","state",
 *        "reason","forced","since_utc"|null,"brownouts_6h","exit_in_s"|null,
 *        "qualifying_s","probe","next_probe_s"|null,"backoff_level",
 *        "skipped":{"flash_runs","ambit_runs","ota"}}
 *
 * - status is read-only; clear and force change device state (fleet_command
 *   prints MUTATING) and take the maintenance lock like set_timezone.
 * - clear: back to normal, brownout history, forced flag, probation and
 *   backoff level wiped.
 * - force: a FORCED state that never auto-exits; only clear ends it (a linked
 *   brownout can still escalate forced no_flash to hold).
 * - Not retained: a retained force would re-apply after every clear.
 * - Invalid input is refused explicitly: invalid_id, invalid_action,
 *   invalid_state (missing/unknown with force, present with status/clear).
 *   Runtime refusals: busy, queue_unavailable, maintenance_active, no_mem.
 *   A force/clear whose NVS write failed answers ok:false "persist_failed"
 *   with the state after it: unchanged for clear / a force down (relaxations
 *   apply only once a positive read shows them on NVS: then ok), the forced
 *   state for a force up (applied in RAM and RTC memory). A clear / force
 *   down whose outcome is unknown is fenced; if the fence is not confirmed on
 *   NVS either the reply is ok:false "persist_unconfirmed": protected now and
 *   across resets, but a power loss may let the relaxation win (total NVS
 *   failure cannot be made durable).
 *
 * Execution: the esp-mqtt task only parses, validates and copies a bounded
 * request; the power_protect API (mutex, NVS) runs on the maintenance worker.
 * Refusals are written into a stack buffer without allocating; a success
 * reply checks every cJSON operation and degrades to a "no_mem" refusal.
 * All power_protect calls are injected so tests/power_protect_host.c runs
 * this production file with fakes.
 */
#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"
#include "power_protect.h"   /* declarations only: status type + enums */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*reply)(const char *json);
    bool (*submit)(void (*run)(void *), void *arg);
    bool (*maintenance_begin)(void);
    void (*maintenance_end)(void);
    void (*get_status)(power_protect_status_t *out);
    esp_err_t (*force)(pp_state_t state);
    esp_err_t (*clear)(void);
    pp_state_t (*state_now)(void);   /* lock-free, for refusals (optional) */
} power_protect_command_config_t;

void power_protect_command_init(const power_protect_command_config_t *config);
/* Returns false for other commands. Caller still owns root. */
bool power_protect_command_handle(const cJSON *root);

#ifdef __cplusplus
}
#endif
