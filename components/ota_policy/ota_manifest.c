#include "ota_manifest.h"
#include <string.h>

/* Kept immediately after esp_app_desc_t by IDF's .flash.appdesc linker rule.
 * Byte encoding is independent of target ABI, endianness and structure padding. */
const uint8_t ota_storage_manifest[OTA_MANIFEST_BYTES]
    __attribute__((section(".rodata_custom_desc"), used, aligned(4))) = {
    'A','M','B','Y','T','E','M','1', 1,0, 32,0, 9,0, 0,0,
    2,0,0,0, 1,0,0,0, 1,0,0,0, 0,0,0,0
};

static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool valid(const ota_manifest_t *m)
{
    return m && m->written > 0 && m->written <= 31 && m->fallback > 0 &&
           m->fallback <= 31 && !(m->readable & 1U) &&
           (m->readable & (UINT32_C(1) << m->written));
}

bool ota_manifest_decode(const void *bytes, size_t length, ota_manifest_t *out)
{
    if (!bytes || !out || length != OTA_MANIFEST_BYTES) return false;
    const uint8_t *p = bytes;
    if (memcmp(p, "AMBYTEM1", 8) || p[8] != 1 || p[9] || p[10] != 32 || p[11] ||
        p[12] != 9 || p[13] || p[14] || p[15] || u32(p + 28)) return false;
    ota_manifest_t m = {u32(p + 16), u32(p + 20), u32(p + 24)};
    if (!valid(&m)) return false;
    *out = m;
    return true;
}

const char *ota_manifest_decide(const ota_manifest_t *r, const ota_manifest_t *c, uint32_t present)
{
    if (!valid(r)) return "running_manifest_invalid";
    if (!valid(c)) return "candidate_manifest_invalid";
    if (!present || (present & 1U)) return "present_versions_invalid";
    uint32_t needed = present | (UINT32_C(1) << r->written);
    if ((c->readable & needed) != needed) return "candidate_cannot_read_present";
    if (!(r->readable & (UINT32_C(1) << c->written))) return "fallback_cannot_read_written";
    if (c->fallback != r->written) return "qualified_fallback_mismatch";
    return NULL;
}
