#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_MANIFEST_OFFSET 0x120U
#define OTA_MANIFEST_BYTES 32U
#define OTA_STORAGE_V1 (UINT32_C(1) << 1)

typedef struct {
    uint32_t readable;
    uint32_t written;
    uint32_t fallback;
} ota_manifest_t;

bool ota_manifest_decode(const void *bytes, size_t length, ota_manifest_t *out);
/* present is an independent storage fact, not the running writer's version.
 * A6 must supply its durable union before introducing a second disk format. */
const char *ota_manifest_decide(const ota_manifest_t *running,
                                const ota_manifest_t *candidate, uint32_t present);
extern const uint8_t ota_storage_manifest[OTA_MANIFEST_BYTES];
