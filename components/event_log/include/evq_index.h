#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Segment index for the two-media event queue (see docs/evq-sd-overflow.md).
 *
 * Why an index at all: once a rotated ev-<seq>.log may live on flash, on SD, or
 * both, "the file is not on flash" stops meaning "the file is drained". The
 * delivery cursor must be able to tell an SD-resident obligation from a real
 * gap, and a reclaim must be able to prove a verified SD copy exists before the
 * only flash copy is deleted. The index is that proof, kept on power-loss-safe
 * internal littlefs — never on the SD card it describes.
 *
 * Format: append-only text, one state transition per line, each line ending in
 * " *<crc32 hex>\n" so a torn or bit-flipped line is rejected on replay instead
 * of silently changing a file's state. Replay applies lines in order; the RAM
 * table is the fold of every valid line. Compaction rewrites the fold to a .tmp
 * file and renames it over the index (littlefs rename is atomic).
 *
 *   S <seq> <first_id> <last_id> <count> <bytes> <crc>   rotated file registered (FLASH)
 *   P <seq> <cid> <primary> <mirror>                   primary+mirror verified (SPOOLED)
 *   O <seq>                                            flash copy reclaimed (SD_ONLY)
 *   D <seq>                                            own ACK prefix passed EOF (DELIVERED)
 *   R <seq> <off> <remaining>                          obligation left the queue unACKed (REIMPORT)
 *   A <seq>                                            entry retired (archived / reimported / delivered-and-gone)
 *   W <seq> <off>                                      first byte written by this firmware (upgrade watermark)
 *
 * The module is pure C (stdio only) so the host harness compiles it unchanged;
 * locking is the caller's (event_log's s_mtx).
 */

#define EVQ_NAME_MAX 32

typedef enum {
    EVQ_SEG_FLASH = 0,     /* rotated, CRC known, flash copy only (or never spooled) */
    EVQ_SEG_SPOOLED,       /* verified primary + mirror on SD; flash copy kept */
    EVQ_SEG_SD_ONLY,       /* flash copy reclaimed; SD copies are the queue's source */
    EVQ_SEG_DELIVERED,     /* own ACK prefix passed EOF; awaiting archive */
    EVQ_SEG_REIMPORT,      /* must be re-appended from SD into the flash tail */
} evq_seg_state_t;

typedef struct {
    uint32_t seq;
    int64_t  first_id;
    int64_t  last_id;
    uint32_t count;        /* complete records */
    uint32_t bytes;
    uint32_t crc;          /* CRC32 of the whole file */
    uint8_t  state;        /* evq_seg_state_t */
    uint32_t cid;          /* SD card serial holding the copies (SPOOLED/SD_ONLY/...) */
    char     primary[EVQ_NAME_MAX];   /* basename in the primary dir, "" if none */
    char     mirror[EVQ_NAME_MAX];    /* basename in the mirror dir, "" if none */
    uint32_t reimport_off;            /* REIMPORT: byte offset to resume from */
    int64_t  reimport_remaining;      /* REIMPORT: records at/after reimport_off, -1 unknown */
    /* RAM-only (never persisted) */
    bool     flash_present;
    uint8_t  flash_crc_checked;       /* 0 unknown, 1 ok, 2 mismatch (this boot) */
    uint32_t sd_verified_epoch;       /* SD mount epoch in which a copy last verified */
    uint8_t  sd_verified_which;       /* 1 primary, 2 mirror */
} evq_seg_t;

typedef struct {
    evq_seg_t *segs;       /* sorted by seq */
    size_t     n;
    size_t     cap;
    bool       have_watermark;
    uint32_t   wm_seq;
    uint32_t   wm_off;
    uint32_t   lines;      /* valid lines in the on-disk index */
    uint32_t   bad_lines;  /* CRC/format-rejected lines seen on replay */
    size_t     file_bytes; /* current index file size (compaction trigger) */
} evq_index_t;

uint32_t evq_crc32(uint32_t crc, const void *buf, size_t len);

/* Table */
esp_err_t  evq_index_init(evq_index_t *ix, size_t cap);
void       evq_index_free(evq_index_t *ix);
evq_seg_t *evq_index_find(evq_index_t *ix, uint32_t seq);
evq_seg_t *evq_index_add(evq_index_t *ix, uint32_t seq);   /* NULL when at cap */
void       evq_index_remove(evq_index_t *ix, uint32_t seq);

/* Persistence. load: replay `path` into ix (missing file = empty, ESP_OK).
 * append: format one line, write + fflush + fsync; ESP_OK only once durable.
 * compact: rewrite the fold to `tmp_path`, fsync, rename over `path`. */
esp_err_t evq_index_load(evq_index_t *ix, const char *path);
esp_err_t evq_index_append(evq_index_t *ix, const char *path, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
esp_err_t evq_index_compact(evq_index_t *ix, const char *path, const char *tmp_path);

/* Line codec (exposed for the host oracle's cross-check and unit tests). */
int  evq_index_format_line(char *dst, size_t cap, const char *body);
bool evq_index_parse_line(const char *line, char *body, size_t body_cap);
/* Apply one validated body to the table (replay and live updates share this). */
bool evq_index_apply(evq_index_t *ix, const char *body);

const char *evq_seg_state_name(uint8_t state);

#ifdef __cplusplus
}
#endif
