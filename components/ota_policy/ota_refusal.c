#include "ota_policy.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_ota_ops.h"

#ifndef OTA_REFUSAL_DIRECTORY
#define OTA_REFUSAL_DIRECTORY "/littlefs/ota"
#endif
#define RECORD_PATH OTA_REFUSAL_DIRECTORY "/refusal.bin"
#define TEMP_PATH OTA_REFUSAL_DIRECTORY "/refusal.tmp"
#define RECORD_BYTES 192U

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (unsigned j = 0; j < 8; ++j) c = (c >> 1) ^ (UINT32_C(0xedb88320) & (0U - (c & 1U)));
    }
    return ~c;
}

static void put32(uint8_t *p, uint32_t n)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void text(uint8_t *p, size_t cap, const char *s)
{
    /* Diagnostics accept only bounded ASCII; JSON emission escapes quotes.
     * Never persist URLs, Wi-Fi settings, credentials or certificate bytes. */
    for (size_t i = 0; s && s[i] && i + 1 < cap; ++i) {
        unsigned char c = (unsigned char)s[i];
        p[i] = c >= 32 && c <= 126 ? c : '?';
    }
}

bool ota_refusal_save(const char *id, const char *source, const char *reason)
{
    uint8_t record[RECORD_BYTES] = {'O','T','A','R','E','F','0','1', 192,0,1,0};
    text(record + 16, 72, id);
    text(record + 88, 12, source);
    text(record + 100, 64, reason);
    put32(record + 188, crc32(record, 188));
    if (mkdir(OTA_REFUSAL_DIRECTORY, 0700) && errno != EEXIST) return false;
    FILE *f = fopen(TEMP_PATH, "wb");
    if (!f) return false;
    bool ok = fwrite(record, 1, sizeof record, f) == sizeof record;
    if (ok && fflush(f)) ok = false;
    if (ok && fsync(fileno(f))) ok = false;
    if (fclose(f)) ok = false;
    if (ok && rename(TEMP_PATH, RECORD_PATH)) ok = false;
    /* Keep a failed temp for diagnosis; the next attempt replaces only temp,
     * never the old committed refusal or another component's files. */
    return ok;
}

static const char *load(uint8_t record[RECORD_BYTES])
{
    FILE *f = fopen(RECORD_PATH, "rb");
    if (!f) return errno == ENOENT ? "missing" : "unavailable";
    bool ok = fread(record, 1, RECORD_BYTES, f) == RECORD_BYTES;
    if (ok && fgetc(f) != EOF) ok = false;
    if (ferror(f)) ok = false;
    if (fclose(f)) ok = false;
    if (!ok) return "unavailable";
    if (memcmp(record, "OTAREF01", 8) || record[8] != 192 || record[9] || record[10] != 1 ||
        record[11] || get32(record + 188) != crc32(record, 188) ||
        !memchr(record + 16, 0, 72) || !memchr(record + 88, 0, 12) || !memchr(record + 100, 0, 64))
        return "corrupt";
    return "available";
}

static void json_string(FILE *f, const uint8_t *s)
{
    fputc('"', f);
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s >= 32 && *s <= 126 ? *s : '?', f);
    }
    fputc('"', f);
}

void ota_policy_print(FILE *f)
{
    uint8_t record[RECORD_BYTES];
    const char *state = load(record);
    ota_manifest_t m;
    bool valid = ota_policy_read_manifest(esp_ota_get_running_partition(), &m);
    flockfile(f);
    fputs("policy: {\"schema\":1,\"readable\":", f);
    if (valid) {
        bool comma = false;
        fputc('[', f);
        for (unsigned i = 1; i <= 31; ++i) if (m.readable & (UINT32_C(1) << i)) {
            fprintf(f, "%s%u", comma ? "," : "", i); comma = true;
        }
        fprintf(f, "],\"written\":%lu,\"fallback\":%lu", (unsigned long)m.written, (unsigned long)m.fallback);
    } else fputs("null,\"written\":null,\"fallback\":null", f);
    fprintf(f, ",\"refusal_state\":\"%s\",\"last_refusal\":", state);
    if (!strcmp(state, "available")) {
        fputs("{\"id\":", f); json_string(f, record + 16);
        fputs(",\"source\":", f); json_string(f, record + 88);
        fputs(",\"reason\":", f); json_string(f, record + 100);
        fputc('}', f);
    } else fputs("null", f);
    fputs("}\r\n", f);
    fflush(f);
    funlockfile(f);
}
