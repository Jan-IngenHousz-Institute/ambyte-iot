#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* pong reply. reset_reason / boot_count (remote diagnostics, 2026-11) are
 * additive: why this boot started (diag_reset_reason_name) and the NVS boot
 * counter (0 = unavailable). NULL reset_reason omits both. power_protect
 * (brownout protection, 2026-11) is additive too: normal | no_flash | hold;
 * NULL omits it. */
bool command_router_pong_json(char *out, size_t capacity, const char *id,
                              const char *device_id, const char *fw, int64_t uptime_ms,
                              const char *reset_reason, uint32_t boot_count,
                              const char *power_protect);
