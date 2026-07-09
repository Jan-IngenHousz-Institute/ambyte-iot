#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

/* Spawn the Lua runner task. Loads /sdcard/main.lua, runs it once. The task
 * self-deletes when the script returns or is stopped. Returns
 * ESP_ERR_INVALID_STATE if a task is already running. */
esp_err_t lua_runner_start(void);

/* Signal the running Lua task to exit and wait up to wait_ms for it to clean
 * up. The script is interrupted via a Lua debug hook (at the next bytecode
 * boundary) and device.sleep_ms wakes early. ESP_OK if the task stopped
 * within the budget, ESP_ERR_TIMEOUT if it is still blocked in a C call
 * (e.g. a long UART read); the task will continue and exit on its own once
 * that call returns. Safe to call when no task is running (no-op). */
esp_err_t lua_runner_stop(uint32_t wait_ms);

/* True while the script task exists (main.lua loaded and not yet finished/
 * stopped). Probe for the status-LED blinker's "measuring" state. */
bool lua_runner_is_running(void);

/* Request a full GC in the Lua VM, serviced at the next debug-hook tick in the
 * Lua task's context. Safe to call from any task (sets a flag only). Wired into
 * device_commands as request_gc so a large MQTT publish can de-fragment the
 * shared heap before allocating its outbox + TLS buffers. */
void lua_runner_request_gc(void);

/* Execute a Lua snippet in a fresh, ephemeral state with the full ambyte env
 * (device/uart/db/ambit/sync globals; no `sched`). Runs in the CALLER's task,
 * in parallel with a running main.lua — remote-CLI parity, not a debugger: it
 * cannot see the running script's variables. UART transactions and the shared
 * ambit payload buffer are serialized at the C layer. Blocks until the snippet
 * finishes or `timeout_ms` expires (0 = no limit; snippets may legitimately run
 * multi-second AMBIT measurements). The first returned value (tostring'd) or
 * the error message is written to `result` (NUL-terminated, truncated to
 * result_cap; pass NULL/0 to discard). Returns ESP_OK on success,
 * ESP_ERR_INVALID_ARG on empty code / syntax error, ESP_FAIL on a runtime
 * error or timeout, ESP_ERR_NO_MEM if the state could not be created.
 * NOTE: device.sleep_ms in a snippet honors the runner's stop signal, so an
 * exec sleeping through a lua_runner_stop() window aborts early. */
esp_err_t lua_runner_exec(const char *code, uint32_t timeout_ms,
                          char *result, size_t result_cap);
