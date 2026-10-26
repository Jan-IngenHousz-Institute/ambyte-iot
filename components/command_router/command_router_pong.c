#include "command_router_pong.h"
#include "ota_identity.h"
#include <stdio.h>

bool command_router_pong_json(char *out, size_t capacity, const char *id,
                              const char *device_id, const char *fw, int64_t uptime_ms)
{
    char firmware_identity[192];
    if (!ota_running_identity_json(firmware_identity, sizeof firmware_identity)) return false;
    int n = snprintf(out, capacity,
             "{\"type\":\"pong\",\"id\":\"%s\",\"device_id\":\"%s\",\"fw\":\"%s\",\"uptime_ms\":%lld,%s}",
             id ? id : "", device_id ? device_id : "", fw ? fw : "",
             (long long)uptime_ms, firmware_identity);
    return n >= 0 && (size_t)n < capacity;
}
