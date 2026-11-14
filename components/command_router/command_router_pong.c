#include "command_router_pong.h"
#include "ota_identity.h"
#include <stdio.h>

bool command_router_pong_json(char *out, size_t capacity, const char *id,
                              const char *device_id, const char *fw, int64_t uptime_ms,
                              const char *reset_reason, uint32_t boot_count)
{
    char firmware_identity[192];
    if (!ota_running_identity_json(firmware_identity, sizeof firmware_identity)) return false;
    char boot[64] = "";
    if (reset_reason != NULL) {
        /* reset_reason is a fixed lowercase enum (diag_reset.h): no escaping. */
        int b = snprintf(boot, sizeof boot, "\"reset_reason\":\"%s\",\"boot_count\":%lu,",
                         reset_reason, (unsigned long)boot_count);
        if (b < 0 || (size_t)b >= sizeof boot) return false;
    }
    int n = snprintf(out, capacity,
             "{\"type\":\"pong\",\"id\":\"%s\",\"device_id\":\"%s\",\"fw\":\"%s\",\"uptime_ms\":%lld,%s%s}",
             id ? id : "", device_id ? device_id : "", fw ? fw : "",
             (long long)uptime_ms, boot, firmware_identity);
    return n >= 0 && (size_t)n < capacity;
}
