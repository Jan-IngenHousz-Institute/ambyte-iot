/* Deterministic synthetic payload (see evq_hil_payload.h). Pure C. */
#include "evq_hil_payload.h"

#include <stdio.h>
#include <string.h>

uint64_t evq_hil_fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; s != NULL && *s != '\0'; s++) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static int run_ok(const char *run)
{
    if (run == NULL || run[0] == '\0') return 0;
    for (const char *p = run; *p != '\0'; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

size_t evq_hil_payload(const char *run, uint32_t k, size_t pad_len, char *out, size_t cap)
{
    static const char set[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    if (out == NULL || !run_ok(run)) return 0;
    int pre = snprintf(out, cap, "{\"evq_hil\":\"%s\",\"k\":%u,\"pad\":\"", run, (unsigned)k);
    if (pre < 0 || (size_t)pre + pad_len + 3 > cap) return 0;
    uint64_t x = evq_hil_fnv1a64(run) ^ ((uint64_t)k * 0x9E3779B97F4A7C15ULL);
    if (x == 0) x = 1;
    char *p = out + pre;
    for (size_t i = 0; i < pad_len; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        p[i] = set[x % 36U];
    }
    memcpy(p + pad_len, "\"}", 3);
    return (size_t)pre + pad_len + 2;
}
