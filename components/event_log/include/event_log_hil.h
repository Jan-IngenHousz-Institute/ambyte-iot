#pragma once

/* HIL-only controls and read-only inventories of the event store
 * (CONFIG_AMBYTE_EVQ_HIL; docs/evq-sd-overflow-hil-contract.md). Nothing here
 * exists in a release build: every declaration is compiled only with the
 * flag, and ISO-1 asserts the release ELF carries none of these symbols.
 *
 * State that must survive the injected CPU/ROM resets (the SD boot hold, the
 * publish-gate override) lives in RTC_NOINIT memory guarded by a magic: a
 * power-on (or any reset that loses RTC RAM) comes back HELD with the gate
 * on HOLD — the safe default for a verification run. */

#ifndef EVQ_HIL_HOST
#include "sdkconfig.h"
#endif

#if CONFIG_AMBYTE_EVQ_HIL || defined(EVQ_HIL_HOST)

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { EVQ_HIL_GATE_AUTO = 0, EVQ_HIL_GATE_HOLD = 1, EVQ_HIL_GATE_FORCE = 2 } evq_hil_gate_t;

/* First statement of app_main: validates the RTC state (magic), installs the
 * SD hold unless a previous boot released it, and defaults the gate to HOLD
 * after a power-on. Safe before any other init. */
void event_log_hil_boot_init(void);

bool event_log_hil_held(void);
void event_log_hil_release(void);           /* clears the SD hold (persists across CPU resets) */
evq_hil_gate_t event_log_hil_gate(void);
void event_log_hil_set_gate(evq_hil_gate_t g);
void event_log_hil_keeper_pause(bool pause);
bool event_log_hil_keeper_paused(void);
void event_log_hil_set_reserve(uint64_t bytes);   /* 0 = production EVQ_SD_RESERVE_BYTES */
uint64_t event_log_hil_reserve(void);
void event_log_hil_set_cid(uint32_t cid);          /* 0 = real card CID */
uint32_t event_log_hil_cid(void);

/* sha256 of the exact line event_log_store_event last wrote for `id` (kept for
 * the 8 most recent stores). */
bool event_log_hil_line_sha(int64_t id, uint8_t out[32], uint32_t *out_bytes);

/* Read-only dumps (printf, one machine line per item). flash_inv refuses
 * unless the keeper is paused or the SD is held. */
esp_err_t event_log_hil_flash_inv(void);
esp_err_t event_log_hil_index_dump(void);
esp_err_t event_log_hil_cursor_dump(void);
esp_err_t event_log_hil_claims_dump(void);
esp_err_t event_log_hil_io_dump(bool drain_trace);
esp_err_t event_log_hil_state_dump(void);

/* Arm one fault at a named EVQ_FAULT_POINT: mode "reset" | "reset_inside" |
 * "eio" | "enospc"; nth = which hit fires (1 = next). */
esp_err_t event_log_hil_arm(const char *point, const char *mode, unsigned nth);

/* Boot-order trace line: "HIL_TRACE <step> <esp_timer µs>". */
void evq_hil_trace(const char *step);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_AMBYTE_EVQ_HIL */
