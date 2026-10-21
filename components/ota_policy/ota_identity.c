#include "ota_identity.h"
#include "ota_manifest.h"
#include "sdkconfig.h"
#include <stdio.h>

bool ota_running_identity_json(char *out, size_t capacity)
{
    ota_manifest_t manifest;
    const char *signed_value = CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT ? "true" : "false";
    int n = snprintf(out, capacity, "\"signed\":%s,\"storage\":", signed_value);
    if (n < 0 || (size_t)n >= capacity) return false;
    size_t used = (size_t)n;
    if (!ota_manifest_decode(ota_storage_manifest, OTA_MANIFEST_BYTES, &manifest)) {
        n = snprintf(out + used, capacity - used, "null");
        return n >= 0 && (size_t)n < capacity - used;
    }
    n = snprintf(out + used, capacity - used, "{\"readable\":[");
    if (n < 0 || (size_t)n >= capacity - used) return false;
    used += (size_t)n;
    bool first = true;
    for (unsigned version = 1; version <= 31; ++version) {
        if (!(manifest.readable & (UINT32_C(1) << version))) continue;
        n = snprintf(out + used, capacity - used, "%s%u", first ? "" : ",", version);
        if (n < 0 || (size_t)n >= capacity - used) return false;
        used += (size_t)n;
        first = false;
    }
    n = snprintf(out + used, capacity - used, "],\"written\":%u,\"fallback\":%u}",
                 (unsigned)manifest.written, (unsigned)manifest.fallback);
    return n >= 0 && (size_t)n < capacity - used;
}
