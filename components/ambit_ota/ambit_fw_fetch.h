#pragma once

/* Remote fetch of an AMBIT release into the SD recovery folder /sdcard/ambit_fw/<ver>/.
 *
 * Why: `ambit_flash` (the ROM-bootloader reflash, the ONLY update path for pre-1.0
 * AMBITs, which have no app-level OTA receiver) reads its four region images from
 * the SD card, and nothing on the device could put them there: a field unit with a
 * legacy AMBIT needed someone to pull its SD card. With a manifest_url the
 * ambit_flash command now downloads the release itself first.
 *
 * Order, chosen so that every state a reset can leave behind is either
 * unflashable or correct (the gate in ambit_fw_manifest.h):
 *   1. GET manifest.json into RAM (≤ AMBIT_FW_MANIFEST_MAX) and parse/bound-check it;
 *      its version must be the one requested.
 *   2. Remove every region file already in the folder that does not match the new
 *      manifest (a missing file fails every preflight closed; a matching one is
 *      kept and not downloaded again).
 *   3. Write manifest.json (replacing a different one): BEFORE any region, so a
 *      folder with a partially written region always carries the manifest that
 *      rejects it.
 *   4. For each missing region: GET <manifest dir>/<asset>, stream it into its final
 *      name by exclusive create (never truncating anything), SHA-256 while writing,
 *      fflush + fsync + checked fclose, then re-read and verify size + SHA-256. A
 *      failed file is removed.
 *   5. Re-verify the whole folder (deep gate). Only then is the fetch ESP_OK.
 * Every file is single-named and never renamed (no FAT rename pair, so removing one
 * can never free another name's chain: the SD-integrity rule in CLAUDE.md).
 *
 * Pure C over stdio + callbacks: tests/ambit_host compiles it unmodified with a
 * scripted HTTP source and the SD fault shim. */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMBIT_FW_FETCH_URL_MAX 256

typedef struct {
    void *ctx;
    /* Start a GET of `url` (following redirects); on a final 200 return a handle and
     * set *clen to Content-Length (-1 unknown). NULL on any failure. */
    void *(*open)(void *ctx, const char *url, int64_t *clen);
    int   (*read)(void *h, uint8_t *buf, size_t cap);   /* >0 bytes, 0 end, <0 error */
    void  (*close)(void *h);
} ambit_fw_http_t;

typedef struct {
    const char *why;        /* "ok" or the first failure (static string) */
    unsigned    downloaded; /* region files written by this attempt */
    unsigned    reused;     /* region files already present and matching */
    unsigned    removed;    /* stale/mismatching region files removed */
    uint32_t    bytes;      /* region bytes downloaded */
} ambit_fw_fetch_result_t;

/* Fetch the release described by `manifest_url` into <root>/<mj>.<mn>.<bt>/.
 * Takes SD io refs itself (never across an HTTP open). ESP_OK only when the folder
 * passed the deep manifest check. */
esp_err_t ambit_fw_fetch(const char *root, const char *manifest_url, uint8_t mj, uint8_t mn, uint8_t bt,
                         const ambit_fw_http_t *http, ambit_fw_fetch_result_t *res);

#ifdef __cplusplus
}
#endif
