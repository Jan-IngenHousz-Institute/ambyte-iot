#pragma once

/* AMBIT release manifest (manifest.json) and the SD recovery-folder gate.
 *
 * Every ambit-iot release publishes manifest.json next to its four ROM images:
 *     {"version":"1.4.0","chip":"esp32c3","flash":[{"file","offset","size","sha256"} x4],...}
 * It is the release's own statement of what each region must contain, so it is the
 * only thing a remote fetch (ambit_fw_fetch) can verify downloaded bytes against.
 *
 * Why a gate: the ROM flash path (ambit_flash_image: remote ambit_flash, the CLI
 * and the boot-time auto-sync) used to accept ANY /sdcard/ambit_fw/<ver>/ whose four
 * files exist and are non-empty. Hand-copied folders were the only source, so that was
 * enough. A folder written by a download can be interrupted (CPU reset, power cut,
 * card park) with app.bin half-written: and boot sync would then auto-flash the
 * truncated image onto every stale AMBIT on the next boot. So:
 *   - a folder WITH manifest.json is flashable only when every region file matches it
 *     (size, and SHA-256 for the deep check that runs before the chip is touched);
 *   - a folder WITHOUT manifest.json is a legacy hand-staged folder and keeps the old
 *     present-and-non-empty rule (operators who copy a release by hand can include
 *     manifest.json to get the same protection).
 * The fetch writes manifest.json BEFORE any region file, so every partially written
 * folder it can leave behind is a gated one.
 *
 * Region bounds are enforced at parse time: partitions.bin must fit before NVS at
 * 0x9000 (an oversized table would overwrite the per-unit calibration the ROM flash
 * promises to preserve), the bootloader before 0x8000, otadata in 0x2000.
 *
 * Pure C over stdio + cJSON + mbedTLS: tests/ambit_host compiles it unmodified.
 * Callers hold an SD io ref (sdcard_io_begin) around the file helpers. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMBIT_FW_MANIFEST_NAME  "manifest.json"
#define AMBIT_FW_MANIFEST_MAX   4096u     /* the v1.4.0 manifest is 836 B */
#define AMBIT_FW_NUM_REGIONS    4u
#define AMBIT_FW_SRC_MAX        64u       /* release asset name, incl. NUL */

typedef struct {
    uint32_t size;
    uint8_t  sha256[32];
    char     src[AMBIT_FW_SRC_MAX];   /* release asset name the bytes are downloaded from */
} ambit_fw_region_t;

/* region[i] is the file ambit_fw_region_name(i) at ambit_fw_region_offset(i). */
typedef struct {
    uint8_t           major, minor, batch;
    ambit_fw_region_t region[AMBIT_FW_NUM_REGIONS];
} ambit_fw_manifest_t;

/* SD file name / flash offset of region i (bootloader, partitions, boot_app0, app) -
 * the same table ambit_flash.c writes (tests/test_ambit_fw_fetch.py asserts it). */
const char *ambit_fw_region_name(size_t i);
uint32_t    ambit_fw_region_offset(size_t i);

/* Parse + bound-check a manifest. ESP_OK fills `out`; otherwise `*why` (static
 * string) names the first problem. Exactly the four known offsets, each once. */
esp_err_t ambit_fw_manifest_parse(const char *json, size_t len, ambit_fw_manifest_t *out, const char **why);

typedef enum {
    AMBIT_FW_FILE_OK = 0,
    AMBIT_FW_FILE_MISSING,       /* no such file */
    AMBIT_FW_FILE_MISMATCH,      /* size (or, deep, SHA-256) differs from the manifest */
    AMBIT_FW_FILE_UNREADABLE,    /* open/read error other than ENOENT */
} ambit_fw_file_t;

/* Check <dir>/<region name i> against `r`. deep = also hash every byte. */
ambit_fw_file_t ambit_fw_file_check(const char *dir, size_t i, const ambit_fw_region_t *r, bool deep);

typedef enum {
    AMBIT_FW_DIR_LEGACY = 0,     /* no manifest.json: hand-staged folder, old rules apply */
    AMBIT_FW_DIR_VERIFIED,       /* manifest present and every region file matches it */
    AMBIT_FW_DIR_REJECTED,       /* manifest present but invalid, or a region doesn't match */
    AMBIT_FW_DIR_UNREADABLE,     /* SD read error: state unknown, treat as not flashable */
} ambit_fw_dir_t;

/* The folder gate. When `dir`'s last path component is a canonical M.m.b, the
 * manifest must name that same version. `*why` (nullable) names the reason. */
ambit_fw_dir_t ambit_fw_dir_check(const char *dir, bool deep, const char **why);

/* Read <dir>/manifest.json into a malloc'd NUL-terminated buffer (≤ MANIFEST_MAX).
 * ESP_ERR_NOT_FOUND when absent; ESP_ERR_INVALID_SIZE when larger than the cap. */
esp_err_t ambit_fw_manifest_read(const char *dir, char **out, size_t *len);

#ifdef __cplusplus
}
#endif
