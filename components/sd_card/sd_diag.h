#pragma once

/* sd_diag — SD write-fault and store-refusal attribution that never depends
 * on the SD card it describes.
 *
 * Why it exists: the SD writers used to fail silently (sd_logger ignored its
 * sync results) or record only "something failed" in RAM (the event store's
 * refusal counters and the keeper's pass-error flag). A fault in the minutes
 * before a reboot was therefore never reported, and the one diagnostic sink
 * that wrote to durable storage — the SD log — is exactly what a failing card
 * cannot hold. These counters live in RTC_NOINIT memory (continuous across CPU
 * resets: SW, panic, watchdogs, and brown-out when the CRC survives) and are
 * snapshotted to NVS on internal flash at a bounded rate. Nothing here ever
 * touches /sdcard.
 *
 * Exactness semantics (diag_exact), fail-closed:
 *   0 — every power-on (or corrupt RTC block) breaks continuity: the counters
 *       are the last NVS snapshot (a FLOOR, if one could be read) plus what
 *       this boot counted; faults between that snapshot and the power loss
 *       are unknown. The epoch number increments at every such break. No NVS
 *       state (missing, unreadable, malformed, or a failed write) can ever
 *       raise it back to 1 — absence of evidence is not proof of zero.
 *   1 — only a block that was already exact and survived a CPU reset with a
 *       valid CRC stays exact; a power-on never produces one.
 *
 * The pure-C core (sd_diag_core.c) is host-compiled by tests/sd_diag_host.c;
 * sd_diag.c is the ESP-IDF glue (RTC placement, reset reason, NVS, locking). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SD_DIAG_W_EVLOG = 0,     /* event_log keeper: spool/mirror/archive/import */
    SD_DIAG_W_SDLOG,         /* sd_logger log tee */
    SD_DIAG_W_AMBIT_OTA,     /* AMBIT OTA staging download */
    SD_DIAG_W_AMBIT_FLASH,   /* AMBIT recovery-dir preflight */
    SD_DIAG_W_COUNT
} sd_diag_writer_t;

typedef enum {
    SD_DIAG_OP_OPEN = 0, SD_DIAG_OP_WRITE, SD_DIAG_OP_FLUSH, SD_DIAG_OP_FSYNC,
    SD_DIAG_OP_CLOSE, SD_DIAG_OP_TRUNCATE, SD_DIAG_OP_RENAME, SD_DIAG_OP_REMOVE,
    SD_DIAG_OP_MKDIR, SD_DIAG_OP_STAT, SD_DIAG_OP_VERIFY, SD_DIAG_OP_READ,
    SD_DIAG_OP_COUNT
} sd_diag_op_t;

/* Store refusal reasons (mirror event_log's refused_* split). */
typedef enum {
    SD_DIAG_REF_FULL = 0, SD_DIAG_REF_MEDIA, SD_DIAG_REF_TOO_LARGE, SD_DIAG_REF_UNAVAILABLE,
    SD_DIAG_REF_COUNT
} sd_diag_refusal_t;

#define SD_DIAG_MAGIC   0x53444447u   /* 'SDDG' */
#define SD_DIAG_VERSION 1u

/* Fixed layout, ≤ 256 B (contract budget for .rtc_noinit). Counters saturate
 * instead of wrapping: a wrapped counter would under-report. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t epoch;
    uint32_t boot_seq;
    uint32_t gen;                 /* bumps on every change; snapshot "changed" test */
    uint8_t  exact;
    uint8_t  floor_pending;       /* power-on boot: NVS floor not merged yet */
    uint8_t  pad[2];
    uint16_t cnt[SD_DIAG_W_COUNT][SD_DIAG_OP_COUNT];
    struct {
        uint8_t  writer, op;
        int16_t  err;
        uint32_t uptime_ms;
        uint32_t boot_seq;
    } last;
    uint32_t refused[SD_DIAG_REF_COUNT];
    int64_t  ref_first_id;        /* first refused measure_id this epoch (0 = none) */
    int64_t  ref_last_id;
    struct {
        uint8_t  reason, blocked, sd_state, pad;
        int16_t  err;
        int16_t  pad2;
        uint32_t uptime_ms;
        int64_t  wall_ms;
    } ref_last;
    uint32_t crc;                 /* CRC32 over every byte before this field */
} sd_diag_block_t;

/* ── pure core (host-testable) ── */
uint32_t sd_diag_crc(const sd_diag_block_t *b);
bool     sd_diag_valid(const sd_diag_block_t *b);
void     sd_diag_seal(sd_diag_block_t *b);
/* Early boot: `cpu_reset` true for SW/panic/WDT/brown-out style resets.
 * Continues a valid block; otherwise starts a fresh provisional one with
 * floor_pending set (the NVS floor is merged by sd_diag_core_merge_floor). */
void     sd_diag_core_boot(sd_diag_block_t *b, bool cpu_reset);
/* After NVS is up: `floor` = the last snapshot, or NULL when it is missing /
 * unreadable / malformed. Always inexact. No-op unless floor_pending. */
void     sd_diag_core_merge_floor(sd_diag_block_t *b, const sd_diag_block_t *floor);
void     sd_diag_core_fault(sd_diag_block_t *b, sd_diag_writer_t w, sd_diag_op_t op, int err, uint32_t uptime_ms);
void     sd_diag_core_refusal(sd_diag_block_t *b, sd_diag_refusal_t reason, int64_t id, int err,
                              uint8_t blocked, uint8_t sd_state, int64_t wall_ms, uint32_t uptime_ms);
/* Snapshot policy: only when changed; the first change of a boot at once,
 * then at most once per SD_DIAG_PERSIST_MIN_MS; `force` (orderly reboot)
 * bypasses the interval but still requires a change. */
#define SD_DIAG_PERSIST_MIN_MS 600000u
bool     sd_diag_core_should_persist(const sd_diag_block_t *b, uint32_t persisted_gen, bool persisted_this_boot,
                                     uint32_t last_persist_ms, uint32_t now_ms, bool force);
const char *sd_diag_writer_name(unsigned w);
const char *sd_diag_op_name(unsigned op);
const char *sd_diag_refusal_name(unsigned r);
/* Render the non-zero counters + last fault + refusal record as one JSON
 * object (no trailing newline). Returns length, or -1 if it would not fit. */
int      sd_diag_render_json(const sd_diag_block_t *b, char *buf, size_t cap);

/* ── device API (sd_diag.c) ── */
void sd_diag_boot_early(void);          /* app_main entry, before any SD writer */
void sd_diag_boot_nvs(void);            /* after nvs_flash_init */
void sd_diag_fault(sd_diag_writer_t w, sd_diag_op_t op, int err);
void sd_diag_refusal(sd_diag_refusal_t reason, int64_t id, int err, uint8_t blocked, uint8_t sd_state);
void sd_diag_get(sd_diag_block_t *out);
/* Rate-limited NVS snapshot (heartbeat cadence); force on the orderly-reboot
 * path. Serialized end to end: blocks while another snapshot is in flight,
 * then decides afresh. Never called from an ISR or with a spinlock held. */
void sd_diag_persist(bool force);

#ifdef __cplusplus
}
#endif
