/* Host driver for the PRODUCTION components/diag/diag_boot.c (review finding
 * 3): one process = one boot. The coredump partition is a 64 KiB file, NVS is
 * a small file that survives between "boots", and esp_core_dump_image_check
 * is IDF's algorithm (CRC32 over [0, size-4) against the stored LE word).
 * esp_core_dump_get_summary is a fake that reads fixed offsets of a CRC-clean
 * image — it is only ever reached after image_check succeeded, which is the
 * ordering this test pins.
 *
 *   boot_host <partition-file|-> <nvs-file> [reset-reason-int] [nvs-broken]
 * prints {"boot_count":N,"reset_reason":"…","crash":{…},"summary_calls":N}. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "esp_core_dump.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "nvs.h"

/* ── diag_log host port (diag_boot.c installs the hook) ────────────────── */
void diag_port_lock(void) {}
void diag_port_unlock(void) {}
int64_t diag_port_uptime_us(void) { return 0; }
bool diag_port_in_isr(void) { return false; }
void *diag_port_task(void) { return (void *)1; }
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func) { (void)func; return NULL; }

/* ── platform facts ────────────────────────────────────────────────────── */
static int g_reset = 1;
esp_reset_reason_t esp_reset_reason(void) { return (esp_reset_reason_t)g_reset; }

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    default: return "ESP_FAIL";
    }
}

uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len)
{
    crc = ~crc;                                   /* = zlib.crc32 semantics */
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ── coredump partition ────────────────────────────────────────────────── */
static uint8_t g_flash[0x10000];
static bool g_have_part;
static const esp_partition_t g_part = {.address = 0x610000, .size = sizeof g_flash};
static int g_summary_calls;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char *label)
{
    (void)type; (void)subtype; (void)label;
    return g_have_part ? &g_part : NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *part, size_t off, void *dst, size_t size)
{
    if (part != &g_part || off > part->size || size > part->size - off) return ESP_ERR_INVALID_ARG;
    memcpy(dst, g_flash + off, size);
    return ESP_OK;
}

static uint32_t rd32(size_t off)
{
    return (uint32_t)g_flash[off] | (uint32_t)g_flash[off + 1] << 8 |
           (uint32_t)g_flash[off + 2] << 16 | (uint32_t)g_flash[off + 3] << 24;
}

esp_err_t esp_core_dump_image_check(void)
{
    const uint32_t size = rd32(0);
    if (size == 0xFFFFFFFFu || size < 8 || size > sizeof g_flash) return ESP_ERR_INVALID_SIZE;
    return esp_rom_crc32_le(0, g_flash, size - 4) == rd32(size - 4) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t *s)
{
    g_summary_calls++;
    /* Reaching here with a corrupt image would be the ordering bug the
     * production code guards against. */
    if (esp_core_dump_image_check() != ESP_OK) abort();
    memset(s, 0, sizeof *s);
    s->exc_pc = rd32(4);
    memcpy(s->exc_task, g_flash + 8, 15);
    s->ex_info.exc_cause = rd32(24);
    s->ex_info.exc_vaddr = rd32(28);
    s->exc_bt_info.depth = 3;
    for (int i = 0; i < 3; i++) s->exc_bt_info.bt[i] = rd32(32 + 4 * (size_t)i);
    memcpy(s->app_elf_sha256, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 65);
    return ESP_OK;
}

/* ── NVS: "<key> <value>" lines, written on commit ─────────────────────── */
#define NVS_MAX 8
static struct { char key[16]; uint64_t value; } g_nvs[NVS_MAX];
static int g_nvs_n;
static const char *g_nvs_path;
static bool g_nvs_broken;

static int nvs_find(const char *key)
{
    for (int i = 0; i < g_nvs_n; i++) if (strcmp(g_nvs[i].key, key) == 0) return i;
    return -1;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    if (g_nvs_broken || strcmp(ns, "diag") != 0) return ESP_FAIL;
    FILE *f = fopen(g_nvs_path, "r");
    if (f != NULL) {
        unsigned long long v;
        char key[16];
        while (g_nvs_n < NVS_MAX && fscanf(f, "%15s %llu", key, &v) == 2) {
            strcpy(g_nvs[g_nvs_n].key, key);
            g_nvs[g_nvs_n++].value = v;
        }
        fclose(f);
    }
    *out = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { (void)h; }

static esp_err_t get(const char *key, uint64_t *v)
{
    int i = nvs_find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    *v = g_nvs[i].value;
    return ESP_OK;
}

static esp_err_t set(const char *key, uint64_t v)
{
    int i = nvs_find(key);
    if (i < 0) {
        if (g_nvs_n == NVS_MAX) return ESP_FAIL;
        i = g_nvs_n++;
        strcpy(g_nvs[i].key, key);
    }
    g_nvs[i].value = v;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    (void)h;
    uint64_t v;
    esp_err_t e = get(key, &v);
    if (e == ESP_OK) *out = (uint32_t)v;
    return e;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value) { (void)h; return set(key, value); }
esp_err_t nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *out) { (void)h; return get(key, out); }
esp_err_t nvs_set_u64(nvs_handle_t h, const char *key, uint64_t value) { (void)h; return set(key, value); }

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    FILE *f = fopen(g_nvs_path, "w");
    if (f == NULL) return ESP_FAIL;
    for (int i = 0; i < g_nvs_n; i++) fprintf(f, "%s %llu\n", g_nvs[i].key, (unsigned long long)g_nvs[i].value);
    fclose(f);
    return ESP_OK;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    if (strcmp(argv[1], "-") != 0) {
        FILE *f = fopen(argv[1], "rb");
        if (f == NULL) return 2;
        if (fread(g_flash, 1, sizeof g_flash, f) != sizeof g_flash) return 2;
        fclose(f);
        g_have_part = true;
    }
    g_nvs_path = argv[2];
    if (argc > 3) g_reset = atoi(argv[3]);
    g_nvs_broken = argc > 4 && strcmp(argv[4], "nvs-broken") == 0;
    diag_boot_init();
    printf("{\"boot_count\":%lu,\"reset_reason\":\"%s\",\"crash\":%s,\"summary_calls\":%d}\n",
           (unsigned long)diag_boot_count(), diag_reset_reason(), diag_crash_json(), g_summary_calls);
    return 0;
}
