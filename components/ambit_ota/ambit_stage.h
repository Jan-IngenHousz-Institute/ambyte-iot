#pragma once

/* Replacement-safe staging of an AMBIT OTA image on the SD card.
 *
 * Why: the original download opened the FINAL /sdcard/ambit_fw.bin with "wb",
 * which truncated the previous image before a single new byte was verified, and
 * it ignored the close result and never synced — a failed download destroyed the
 * old artifact and could still be reported "downloaded". Now:
 *   - the legacy final path is never opened for writing, renamed or removed;
 *   - each attempt writes a collision-free single-named stage
 *     <dir>/ambit_fw.stg-<k>.bin (k < AMBIT_STAGE_SLOTS, exclusive create),
 *     hashing and counting while it streams, then fflush + fsync + checked
 *     fclose, then reopens and re-hashes every byte;
 *   - the verified (path, length, SHA-256) exists only in the RAM of the attempt
 *     that produced it: nothing ever streams an existing stage file, so a stage
 *     left behind by a CPU reset is untrusted and is removed by the next
 *     attempt's cleanup (step 1) — stages are never renamed, so unlinking one
 *     can never free a cross-linked chain;
 *   - streaming to the AMBIT re-hashes what it reads and aborts instead of
 *     sending OTA_END on any mismatch or read error.
 *
 * Pure C over stdio + callbacks; the host harness (tests/ambit_host) compiles
 * this file unmodified. Caller holds an SD io ref (sdcard_io_begin) around
 * ambit_stage_download / ambit_stage_stream / ambit_stage_remove. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMBIT_STAGE_SLOTS 8

typedef enum {
    AMBIT_STAGE_OK = 0,
    AMBIT_STAGE_SLOTS_BLOCKED,   /* every slot still occupied after cleanup */
    AMBIT_STAGE_OPEN_FAILED,
    AMBIT_STAGE_SOURCE_FAILED,   /* network read error */
    AMBIT_STAGE_SHORT_SOURCE,    /* fewer bytes than the announced length */
    AMBIT_STAGE_WRITE_FAILED,
    AMBIT_STAGE_SYNC_FAILED,     /* fflush or fsync */
    AMBIT_STAGE_CLOSE_FAILED,
    AMBIT_STAGE_READBACK_FAILED, /* could not reopen/read the stage */
    AMBIT_STAGE_MISMATCH,        /* read-back length/SHA differs: stage kept as evidence */
    AMBIT_STAGE_EMPTY,
} ambit_stage_why_t;

typedef struct {
    char     path[96];
    uint8_t  sha256[32];
    size_t   len;
} ambit_stage_t;

typedef struct {
    unsigned stale_removed;      /* step-1 removals this attempt */
    unsigned stale_remove_err;
} ambit_stage_cleanup_t;

/* Source of image bytes: >0 bytes read, 0 end, <0 error. */
typedef int (*ambit_stage_read_fn)(void *ctx, uint8_t *buf, size_t cap);

/* Sink for streaming a verified stage to the target. Each returns true on success. */
typedef struct {
    void *ctx;
    bool (*begin)(void *ctx, size_t len);
    bool (*data)(void *ctx, const uint8_t *buf, size_t n);
    bool (*end)(void *ctx);
    void (*abort)(void *ctx);
} ambit_stage_sink_t;

/* Step 1: remove every stale single-named stage in `dir`. */
void ambit_stage_cleanup(const char *dir, ambit_stage_cleanup_t *out);

/* Cleanup, allocate the lowest free slot, stream `rd` into it and verify.
 * `expect_len` > 0 enforces the announced length. On success `out` holds the
 * verified identity. On any failure the stage is removed, except a read-back
 * MISMATCH, whose bytes are kept for inspection until the next attempt. */
esp_err_t ambit_stage_download(const char *dir, ambit_stage_read_fn rd, void *rd_ctx, int64_t expect_len,
                               ambit_stage_t *out, ambit_stage_why_t *why, ambit_stage_cleanup_t *cleanup);

/* Stream the verified stage to `sink`, re-hashing every byte read. Sends
 * end() only when length and SHA-256 match `st`; otherwise abort(). */
bool ambit_stage_stream(const ambit_stage_t *st, const ambit_stage_sink_t *sink, uint8_t *buf, size_t cap);

/* Remove this attempt's stage (single-named, never renamed). */
bool ambit_stage_remove(const ambit_stage_t *st);

const char *ambit_stage_why_name(ambit_stage_why_t why);

#ifdef __cplusplus
}
#endif
