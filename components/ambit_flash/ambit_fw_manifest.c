/* AMBIT release manifest parse + SD recovery-folder gate (see ambit_fw_manifest.h). */
#include "ambit_fw_manifest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "sd_diag.h"

/* Verification build only: `evq_hil fault io ambit_flash …` (LAST include). */
#define EVQ_HIL_WRITER SD_DIAG_W_AMBIT_FLASH
#include "evq_hil_io_w.h"

#define CHECK_BUF 1024u

/* Offset, SD name and the largest image that fits before the next region. The
 * partitions cap is the load-bearing one: NVS (per-unit calibration) starts at
 * 0x9000 and the ROM flash promises never to write it. The app cap is the 4 MiB
 * part minus the app offset; the release's own partition table bounds it further. */
static const struct {
    uint32_t    offset;
    const char *name;
    uint32_t    max;
} s_layout[AMBIT_FW_NUM_REGIONS] = {
    { 0x0,     "bootloader.bin", 0x8000   },
    { 0x8000,  "partitions.bin", 0x1000   },
    { 0xe000,  "boot_app0.bin",  0x2000   },
    { 0x10000, "app.bin",        0x3F0000 },
};

const char *ambit_fw_region_name(size_t i)
{
    return i < AMBIT_FW_NUM_REGIONS ? s_layout[i].name : NULL;
}

uint32_t ambit_fw_region_offset(size_t i)
{
    return i < AMBIT_FW_NUM_REGIONS ? s_layout[i].offset : 0;
}

/* ── parse ──────────────────────────────────────────────────────────────── */

static bool parse_version(const char *s, uint8_t *mj, uint8_t *mn, uint8_t *bt)
{
    unsigned a = 0, b = 0, c = 0;
    char tail = 0;
    if (s == NULL || sscanf(s, "%u.%u.%u%c", &a, &b, &c, &tail) != 3 || a > 255 || b > 255 || c > 255) {
        return false;
    }
    *mj = (uint8_t)a;
    *mn = (uint8_t)b;
    *bt = (uint8_t)c;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_sha(const char *s, uint8_t out[32])
{
    if (s == NULL || strlen(s) != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* A release asset name becomes the last segment of a download URL: plain
 * [A-Za-z0-9._-], no leading dot, so it can never add a path or a query. */
static bool safe_asset_name(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n >= AMBIT_FW_SRC_MAX || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* "0x10000" (the published form) or a JSON number. */
static bool parse_offset(const cJSON *j, uint32_t *out)
{
    if (cJSON_IsNumber(j)) {
        if (j->valuedouble < 0 || j->valuedouble > 0xFFFFFFFF || j->valuedouble != (double)(uint32_t)j->valuedouble) {
            return false;
        }
        *out = (uint32_t)j->valuedouble;
        return true;
    }
    if (!cJSON_IsString(j) || j->valuestring == NULL) return false;
    const char *s = j->valuestring;
    if (!(s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && s[2] != '\0')) return false;
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s + 2, &end, 16);
    if (errno != 0 || end == NULL || *end != '\0' || v > 0xFFFFFFFFul) return false;
    *out = (uint32_t)v;
    return true;
}

esp_err_t ambit_fw_manifest_parse(const char *json, size_t len, ambit_fw_manifest_t *out, const char **why)
{
    const char *w = "ok";
    esp_err_t rc = ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    cJSON *root = (json != NULL && len > 0) ? cJSON_ParseWithLength(json, len) : NULL;
    bool seen[AMBIT_FW_NUM_REGIONS] = { false };

    if (!cJSON_IsObject(root)) { w = "not_json_object"; goto out; }
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsString(ver) || !parse_version(ver->valuestring, &out->major, &out->minor, &out->batch)) {
        w = "bad_version"; goto out;
    }
    const cJSON *chip = cJSON_GetObjectItemCaseSensitive(root, "chip");
    if (!cJSON_IsString(chip) || strcmp(chip->valuestring, "esp32c3") != 0) { w = "not_esp32c3"; goto out; }
    const cJSON *flash = cJSON_GetObjectItemCaseSensitive(root, "flash");
    if (!cJSON_IsArray(flash) || cJSON_GetArraySize(flash) != (int)AMBIT_FW_NUM_REGIONS) {
        w = "flash_not_four_regions"; goto out;
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, flash) {
        uint32_t off = 0;
        if (!cJSON_IsObject(e) || !parse_offset(cJSON_GetObjectItemCaseSensitive(e, "offset"), &off)) {
            w = "bad_offset"; goto out;
        }
        size_t i = 0;
        while (i < AMBIT_FW_NUM_REGIONS && s_layout[i].offset != off) i++;
        if (i == AMBIT_FW_NUM_REGIONS) { w = "unknown_offset"; goto out; }
        if (seen[i]) { w = "duplicate_offset"; goto out; }
        seen[i] = true;
        ambit_fw_region_t *r = &out->region[i];
        const cJSON *file = cJSON_GetObjectItemCaseSensitive(e, "file");
        if (!cJSON_IsString(file) || !safe_asset_name(file->valuestring)) { w = "bad_file_name"; goto out; }
        strcpy(r->src, file->valuestring);   /* length checked by safe_asset_name */
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(e, "size");
        if (!cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > (double)s_layout[i].max ||
            size->valuedouble != (double)(uint32_t)size->valuedouble) {
            w = "region_size_out_of_bounds"; goto out;
        }
        r->size = (uint32_t)size->valuedouble;
        const cJSON *sha = cJSON_GetObjectItemCaseSensitive(e, "sha256");
        if (!cJSON_IsString(sha) || !parse_sha(sha->valuestring, r->sha256)) { w = "bad_sha256"; goto out; }
    }
    rc = ESP_OK;
out:
    cJSON_Delete(root);
    if (rc != ESP_OK) memset(out, 0, sizeof *out);
    if (why) *why = w;
    return rc;
}

/* ── SD helpers (caller holds an io ref) ────────────────────────────────── */

ambit_fw_file_t ambit_fw_file_check(const char *dir, size_t i, const ambit_fw_region_t *r, bool deep)
{
    if (i >= AMBIT_FW_NUM_REGIONS) return AMBIT_FW_FILE_MISMATCH;
    char path[192];
    snprintf(path, sizeof path, "%s/%s", dir, s_layout[i].name);
    errno = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (errno == ENOENT) return AMBIT_FW_FILE_MISSING;
        sd_diag_fault(SD_DIAG_W_AMBIT_FLASH, SD_DIAG_OP_OPEN, errno);
        return AMBIT_FW_FILE_UNREADABLE;
    }
    ambit_fw_file_t res = AMBIT_FW_FILE_OK;
    if (!deep) {
        long sz = -1;
        if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
        if (sz < 0) res = AMBIT_FW_FILE_UNREADABLE;
        else if ((uint32_t)sz != r->size) res = AMBIT_FW_FILE_MISMATCH;
        fclose(f);
        return res;
    }
    uint8_t *buf = malloc(CHECK_BUF);
    if (buf == NULL) {
        fclose(f);
        return AMBIT_FW_FILE_UNREADABLE;
    }
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    mbedtls_sha256_starts(&h, 0);
    size_t got = 0, n;
    while ((n = fread(buf, 1, CHECK_BUF, f)) > 0) {
        got += n;
        if (got > r->size) break;                       /* longer than the release says */
        mbedtls_sha256_update(&h, buf, n);
    }
    if (ferror(f)) {
        sd_diag_fault(SD_DIAG_W_AMBIT_FLASH, SD_DIAG_OP_READ, EIO);
        res = AMBIT_FW_FILE_UNREADABLE;
    }
    fclose(f);
    free(buf);
    uint8_t have[32];
    mbedtls_sha256_finish(&h, have);
    mbedtls_sha256_free(&h);
    if (res == AMBIT_FW_FILE_OK && (got != r->size || memcmp(have, r->sha256, sizeof have) != 0)) {
        res = AMBIT_FW_FILE_MISMATCH;
    }
    return res;
}

esp_err_t ambit_fw_manifest_read(const char *dir, char **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    char path[192];
    snprintf(path, sizeof path, "%s/%s", dir, AMBIT_FW_MANIFEST_NAME);
    errno = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (errno == ENOENT) return ESP_ERR_NOT_FOUND;
        sd_diag_fault(SD_DIAG_W_AMBIT_FLASH, SD_DIAG_OP_OPEN, errno);
        return ESP_FAIL;
    }
    char *buf = malloc(AMBIT_FW_MANIFEST_MAX + 1);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, AMBIT_FW_MANIFEST_MAX + 1, f);
    bool err = ferror(f) != 0;
    fclose(f);
    if (err) {
        sd_diag_fault(SD_DIAG_W_AMBIT_FLASH, SD_DIAG_OP_READ, EIO);
        free(buf);
        return ESP_FAIL;
    }
    if (n > AMBIT_FW_MANIFEST_MAX) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    buf[n] = '\0';
    *out = buf;
    *len = n;
    return ESP_OK;
}

ambit_fw_dir_t ambit_fw_dir_check(const char *dir, bool deep, const char **why)
{
    const char *w = "ok";
    ambit_fw_dir_t res;
    char *json = NULL;
    size_t len = 0;
    esp_err_t e = ambit_fw_manifest_read(dir, &json, &len);
    if (e == ESP_ERR_NOT_FOUND) {
        if (why) *why = "no_manifest";
        return AMBIT_FW_DIR_LEGACY;
    }
    if (e == ESP_ERR_INVALID_SIZE) {
        if (why) *why = "manifest_too_large";
        return AMBIT_FW_DIR_REJECTED;
    }
    if (e != ESP_OK) {
        if (why) *why = "manifest_unreadable";
        return AMBIT_FW_DIR_UNREADABLE;
    }
    ambit_fw_manifest_t m;
    if (ambit_fw_manifest_parse(json, len, &m, &w) != ESP_OK) {
        free(json);
        if (why) *why = w;
        return AMBIT_FW_DIR_REJECTED;
    }
    free(json);

    /* A folder named M.m.b must hold that version's manifest: a mislabelled copy
     * would make boot sync's version compare (folder name) lie about what it flashes. */
    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    uint8_t mj, mn, bt;
    if (parse_version(base, &mj, &mn, &bt) && (mj != m.major || mn != m.minor || bt != m.batch)) {
        if (why) *why = "manifest_version_mismatch";
        return AMBIT_FW_DIR_REJECTED;
    }

    res = AMBIT_FW_DIR_VERIFIED;
    for (size_t i = 0; i < AMBIT_FW_NUM_REGIONS && res == AMBIT_FW_DIR_VERIFIED; i++) {
        switch (ambit_fw_file_check(dir, i, &m.region[i], deep)) {
        case AMBIT_FW_FILE_OK:         break;
        case AMBIT_FW_FILE_MISSING:    res = AMBIT_FW_DIR_REJECTED;   w = "region_missing";    break;
        case AMBIT_FW_FILE_MISMATCH:   res = AMBIT_FW_DIR_REJECTED;   w = "region_mismatch";   break;
        case AMBIT_FW_FILE_UNREADABLE: res = AMBIT_FW_DIR_UNREADABLE; w = "region_unreadable"; break;
        }
    }
    if (why) *why = w;
    return res;
}
