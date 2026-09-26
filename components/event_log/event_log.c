/*
 * event_log.c — append-only event log behind persistence_port.h.
 *
 * Replaces SQLite (which corrupted FATFS/SD via in-place page + header rewrites)
 * with an append-only store-and-forward FIFO. See event_log.h and
 * docs/evq-sd-overflow.md for the design rationale.
 *
 * Concurrency: every public op runs under s_mtx, serialising the status-heartbeat
 * snapshot/ID allocator, the schedule runner
 * (store), the sync-runner drain (claim plus deferred ack/error marks), the
 * sync-runner watchdog task (cmd_store_status_event/cmd_db_status), the SD
 * keeper task, and the CLI (stats). MQTT/Wi-Fi event tasks never enter this
 * component: device_commands queues their completions for the drain so FATFS
 * latency cannot stall socket servicing. The RAM claim window is bounded by the
 * same 16-slot/64-KiB contract as the publisher; only its contiguous ACKed
 * prefix is durable.
 *
 * TWO MEDIA, ONE QUEUE (2026-09 SD-overflow repair). Until v2.4.2 only fully
 * delivered files ever reached the SD card, so a broker outage longer than the
 * ~17 h internal store filled flash and every later measurement was refused
 * while a 244 GB card sat idle (the nine full gateways of Sep 2026). Now a
 * rotated ev-<seq>.log may live on flash, on SD, or both: unsent files are
 * copied byte-for-byte to SD (a rollback-importable primary plus a mirror),
 * and only once both copies are durable and verified may the flash copy be
 * reclaimed under pressure. The cursor keeps its meaning — offsets are
 * identical on either medium — and claim reads whichever verified copy exists.
 * The segment index (evq_index.c, internal littlefs) is what lets the cursor
 * tell "on an absent card" from "gone": an indexed file is never skipped.
 *
 * Lock order: s_mtx → sdcard_io_begin ref. No blocking lock is ever taken
 * while an SD ref is held (sd_card.h contract). The keeper copies and verifies
 * files WITHOUT s_mtx (rotated files are immutable; a pinned seq is exempt from
 * eviction), so a slow or hung card delays only the keeper, never a store.
 */

#include "event_log.h"
#include "evq_fault.h"
#include "evq_index.h"
#include "sd_card.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "event_log"

/* ── internal-store seams ──────────────────────────────────────────────────
 * The store used to live on the hot-unpluggable SD card, where every FATFS touch
 * needed an unmount-safety refcount (sdcard_io_begin/end) and an error-driven
 * card-loss latch. The internal partition can never be unplugged and its volume
 * is never freed at runtime, so those seams collapse to no-ops — kept as named
 * functions (not deleted call sites) so the store's I/O discipline stays visible
 * and could target removable media again without re-auditing every path. */
static inline bool evstore_io_begin(void) { return true; }
static inline void evstore_io_end(void) {}

/* I/O errors on internal flash are not a removal signal (nothing to unmount) but
 * they are still worth counting: a rising streak means the partition or driver is
 * sick, which should be visible on the bench log rather than silently retried. */
static volatile uint32_t s_evstore_io_errors = 0;
static void evstore_report_io_error(void)
{
    uint32_t n = ++s_evstore_io_errors;
    if (n <= 3 || (n % 100) == 0) {
        ESP_LOGE(TAG, "internal-store I/O error (#%u)", (unsigned)n);
    }
}
static inline void evstore_report_io_ok(void) {}

/* Free/total bytes on the internal store partition (littlefs bookkeeping, no
 * media walk — cheap enough for per-store admission checks). */
static esp_err_t evstore_space(uint64_t *out_free, uint64_t *out_total)
{
    size_t total = 0, used = 0;
    esp_err_t err = esp_littlefs_info(EVSTORE_PARTITION, &total, &used);
    if (err != ESP_OK) return err;
    if (out_free)  *out_free  = (total > used) ? (uint64_t)(total - used) : 0;
    if (out_total) *out_total = (uint64_t)total;
    return ESP_OK;
}

static esp_err_t evstore_free_bytes(uint64_t *out_free)
{
    if (out_free == NULL) return ESP_ERR_INVALID_ARG;
    return evstore_space(out_free, NULL);
}

/* ── layout ────────────────────────────────────────────────────────────────
 * Internal store (littlefs, power-loss-safe):
 *   EVSTORE_MOUNT/events/ev-<seq>.log   the queue's flash segments (tail = max seq)
 *   EVSTORE_MOUNT/events/quarantine.log intact copies of records passed WITHOUT
 *                                       an ACK (publish-impossible poison, malformed
 *                                       pre-upgrade lines, detected corruption)
 *   EVSTORE_MOUNT/evq.idx               segment index (evq_index.h)
 * SD card (FAT, corruption-prone, removable):
 *   EVQ_SD_ROOT/events/ev-<first_id>.log  unsent PRIMARY copy. Deliberately the
 *        directory every firmware since v1.10.0 imports verbatim on its keeper
 *        pass: a rollback re-ingests SD-only records instead of stranding them.
 *   EVQ_SD_ROOT/evq/m-<seq>.log           MIRROR copy (old firmware never reads it)
 *   EVQ_SD_ROOT/archive/arc-<first_id>[-<n>].log  delivered records (bulk archive)
 * The partition label and the /events + /archive names are load-bearing across
 * the fleet; the root prefixes are #ifndef-guarded only so the host harness can
 * point them at a temp directory (check_constants.py pins production values). */
#ifndef EVQ_SD_ROOT
#define EVQ_SD_ROOT          SD_MOUNT_POINT
#endif
#define EVLOG_DIR            EVSTORE_MOUNT "/events"
#define EVLOG_QUARANTINE     EVLOG_DIR "/quarantine.log"
#define EVQ_INDEX_PATH       EVSTORE_MOUNT "/evq.idx"
#define EVQ_INDEX_TMP        EVSTORE_MOUNT "/evq.idx.tmp"
#define EVLOG_LEGACY_SD_DIR  EVQ_SD_ROOT "/events"     /* legacy import dir == spool primary dir */
#define EVQ_MIRROR_DIR       EVQ_SD_ROOT "/evq"
#define EVLOG_ARCHIVE_DIR    EVQ_SD_ROOT "/archive"
#define EVQ_PATH_MAX         112

/* ambit_trace's binding case is the permanent new-firmware v2 fallback:
 * 62,999 payload + 1,535 metadata + 543 arrun command + 113 fixed header +
 * 2 framing = 65,192 B, strictly below EVLOG_RECORD_CAP_NORMAL (65,552) with
 * 360 B spare. Canonical v3 rows leave metadata empty and are smaller. The
 * producer names every term and proves the positive margin with static asserts
 * against this component's public cap (a reverse include here would create an
 * event_log <-> ambit_trace component dependency cycle). */
#ifndef EVLOG_ROTATE_BYTES
#define EVLOG_ROTATE_BYTES   (256 * 1024)     /* roll the tail file past this size */
#endif
/* DURABLE ACCEPTANCE: store_event returns ESP_OK only after the record's bytes
 * are fsync'd. Until v2.4.2 the tail was flushed every 8 records / 1.5 s, so up
 * to 8 records reported "saved" could vanish in a brownout. That batching was
 * sized for FAT, where every fsync rewrote the FAT sector + dir entry in place;
 * on copy-on-write littlefs a sync commits a new metadata pair and cannot tear
 * the volume, so per-record durability costs only flash writes (at field
 * cadence, decades of wear budget on this partition). */
#ifndef EVLOG_ARCHIVE_EVERY_N
#define EVLOG_ARCHIVE_EVERY_N 1000            /* SD transfer burst once per N stores */
#endif
#define EVLOG_CURSOR_BATCH   16               /* persist read cursor every N acks */
#define EVLOG_ID_BLOCK       64               /* reserve next_id in blocks → 1 NVS write / 64 ids */
#define EVLOG_SCAN_MAX_LINES 20000            /* bound an UNINDEXED-file pending count (stat only) */
#define EVLOG_SCAN_MAX_MS    3000             /* …and its wall-clock */
#ifndef EVLOG_MIN_FREE_BYTES
#define EVLOG_MIN_FREE_BYTES (256 * 1024)     /* storage-full watermark: refuse writes below this (audit C1/C2) */
#endif
#ifndef EVLOG_EVICT_TARGET
#define EVLOG_EVICT_TARGET   (512 * 1024)     /* eviction hysteresis: clean synced files until this much is free */
#endif
#define EVLOG_OOM_STUCK_MAX  30               /* NO_MEM retries on one head record → quarantine it (audit D2) */
/* Pressure: below this share of the partition free, the store wakes the keeper
 * at once (not at the next 1000-store burst); the keeper spools and reclaims
 * verified copies oldest-first until EVQ_RECLAIM_PCT is free again. */
#ifndef EVQ_PRESSURE_PCT
#define EVQ_PRESSURE_PCT     25
#endif
#ifndef EVQ_RECLAIM_PCT
#define EVQ_RECLAIM_PCT      40
#endif
/* Never fill the card: sd_logger and AMBIT firmware images share it. */
#ifndef EVQ_SD_RESERVE_BYTES
#define EVQ_SD_RESERVE_BYTES (64ULL * 1024 * 1024)
#endif
#ifndef EVQ_INDEX_CAP
#define EVQ_INDEX_CAP        4096             /* segments (~1 GiB at 256 KiB) — PSRAM table */
#endif
#define EVQ_INDEX_CAP_FALLBACK 512            /* PSRAM-absent boot */
#ifndef EVQ_INDEX_COMPACT_BYTES
#define EVQ_INDEX_COMPACT_BYTES (48 * 1024)
#endif
#ifndef EVQ_KEEPER_PERIOD_MS
#define EVQ_KEEPER_PERIOD_MS 60000
#endif
#ifndef EVQ_SD_BACKOFF_MIN_MS
#define EVQ_SD_BACKOFF_MIN_MS 60000
#endif
#ifndef EVQ_SD_BACKOFF_MAX_MS
#define EVQ_SD_BACKOFF_MAX_MS (30 * 60000)
#endif
#define EVQ_FALLBACK_NAME_BASE 3000000000U    /* primary collision fallback: above any measure_id */

_Static_assert(PUBLISH_WINDOW_SLOTS > 0 && PUBLISH_WINDOW_SLOTS <= 16,
               "publish window must fit the firmware's 16-slot ACK table");
_Static_assert(PUBLISH_WINDOW_BYTES > 0, "publish window byte budget must be non-zero");
_Static_assert(EVQ_RECLAIM_PCT > EVQ_PRESSURE_PCT, "reclaim target must exceed the pressure watermark");

#define NVS_NS               "evlog"
#define NVS_KEY_RD_SEQ       "rd_seq"   /* legacy cursor keys — still written for rollback */
#define NVS_KEY_RD_OFF       "rd_off"
#define NVS_KEY_CUR          "cur"      /* atomic cursor blob {seq, off, crc}; only this firmware writes it */
#define NVS_KEY_NID          "nid"

typedef struct {
    uint32_t seq;
    uint32_t off;
    uint32_t crc;
} evlog_cursor_blob_t;

static SemaphoreHandle_t s_mtx = NULL;
static StaticSemaphore_t s_mtx_storage;
/* volatile: read lock-free by the racy keeper trigger checks. */
static volatile bool s_available = false;
/* Composition-root callback: invariant recovery rebuilds this component's
 * volatile claim window from the durable cursor, so the MQTT peer must discard
 * its RAM correlation table/queue at the same boundary. Stored once during boot. */
static void (*s_reset_notifier)(void) = NULL;
/* Set only under s_mtx when an invariant-recovery path discards the event window.
 * The mutating caller consumes it before releasing s_mtx, then invokes the peer
 * reset outside the lock. This preserves the event_log -> device_commands
 * dependency boundary without leaving the two RAM windows divergent for 60 s. */
static bool s_reset_pending = false;

/* Tail (write) file + its running segment statistics (become the index S line
 * at rotation, so a rotated file's CRC is known without re-reading it). */
static FILE     *s_wf        = NULL;
static uint32_t  s_tail_seq  = 1;
static long      s_tail_size = 0;
static uint32_t  s_tail_count = 0;
static uint32_t  s_tail_crc   = 0;
static int64_t   s_tail_first_id = 0;
static int64_t   s_tail_last_id  = 0;

/* Read cursor (the next record to publish). */
static uint32_t  s_rd_seq = 1;
static long      s_rd_off = 0;
/* Records (complete lines) before s_rd_off in the cursor file — the exact
 * pending count subtracts it. Unknown until the boot offset is validated. */
static uint32_t  s_cur_consumed = 0;
static bool      s_cur_validated = false;

/* RAM-only claim window over the durable read cursor.  Slots are ordered by
 * (seq,off) and remain in this array until they leave the contiguous ACKed
 * prefix.  `claimed=false && !acked` is a reverted record and is always returned
 * before a new offset is admitted; this is what makes a mid-window timeout or
 * disconnect replay FIFO rather than leaking past the gap.  The cursor, rotated
 * file retirement, pending count, and NVS persistence NEVER follow the claim tail.
 * They move only when slot zero is ACKed (or explicitly frontier-quarantined).
 *
 * Per-slot token invariant, paired with device_commands' msg-id table: an
 * unacked claimed record has at most one MQTT latch or one queued completion;
 * event_log itself is mutated only by the sole sync-runner consumer. */
typedef struct {
    int64_t  measure_id;
    uint32_t seq;
    long     off;
    long     len;
    bool     claimed;
    bool     acked;
} evlog_window_slot_t;

static evlog_window_slot_t s_window[PUBLISH_WINDOW_SLOTS];
static size_t              s_window_count = 0;
static size_t              s_window_bytes = 0;

/* next_id: hand out from RAM, persist a high-water mark every EVLOG_ID_BLOCK. */
static int64_t   s_next_id  = 1;
static int64_t   s_id_limit = 1;

/* Segment index (RAM fold of EVQ_INDEX_PATH). */
static evq_index_t s_ix;
static bool        s_ix_ready = false;

/* Unindexed rotated flash files at/after the cursor (pre-upgrade files, or a
 * crash between rotation and its S line). Pending is only a FLOOR while any
 * exist; the keeper indexes them. s_unidx_floor is their bounded line count. */
static bool      s_unidx_present = false;
static int64_t   s_unidx_floor   = 0;
/* Records in UNOWNED /sdcard/events files (pre-internal-store backlog, an
 * index-lost spool, a rollback leftover) that the keeper will import. They are
 * owed, so they count as pending (inside reimport_pending): a card holding
 * them is never reported as an empty backlog. Unknown until the card was
 * scanned in this session → pending is a floor. */
static int64_t   s_legacy_pending = 0;
static bool      s_legacy_known   = false;
static uint32_t  s_legacy_counted_epoch = 0;   /* one full scan per card mount, then decrements */

/* Bookkeeping. */
static uint32_t   s_acks_since_persist = 0;
static uint32_t   s_stores_since_archive = 0;   /* SD transfer burst trigger */

/* Health / loss accounting (event_log_health → STATUS metadata + `evlog`). */
static int64_t   s_skipped       = 0;   /* legacy aggregate: records passed without publish */
static int64_t   s_dropped       = 0;   /* legacy aggregate: records refused at store */
static int64_t   s_last_acked_id = 0;   /* highest measure_id confirmed synced (PUBACK) */
static int64_t   s_refused_full = 0, s_refused_media = 0, s_refused_too_large = 0, s_refused_unavailable = 0;
static int64_t   s_quarantined_poison = 0, s_quarantined_malformed = 0, s_corrupt_detected = 0;
static int64_t   s_skipped_unindexed_gap = 0;
static uint8_t   s_corrupt_medium = EVQ_MEDIUM_NONE;
static uint32_t  s_pressure_notifies = 0, s_sd_bursts = 0;
static uint32_t  s_spool_files = 0, s_spool_errors = 0, s_mirror_used = 0;
static uint32_t  s_reclaimed_files = 0, s_archived_files = 0, s_reimported_files = 0;
static uint32_t  s_sd_bad_copies = 0;          /* damaged SD copies moved aside (kept, never delivered) */
static uint32_t  s_sd_retired_names = 0;       /* .tmp names retired (renamed aside, never unlinked) — on the card now */
/* Archive retry bound (eval R2-F1): consecutive failed archive passes of the
 * same segment. After EVQ_ARCHIVE_TRIES an unreadable SD copy is treated as
 * damaged (moved aside) instead of being retried forever. Keeper-only. */
#define EVQ_ARCHIVE_TRIES 3
static uint32_t  s_arch_fail_seq = 0;
static uint8_t   s_arch_fail_n = 0;
/* Storage-full is distinct from card-loss: a full store is HEALTHY, so we pause
 * writes WITHOUT reporting an I/O error. The drain + keeper free space and
 * store admission re-enables writes. */
static volatile bool s_write_full = false;
/* Head-of-line OOM tracking: a record too big to strdup on the fragmented heap is
 * retried; after EVLOG_OOM_STUCK_MAX strikes on the SAME head it is quarantined so
 * the drain advances instead of reboot-looping. */
static int64_t   s_oom_head_id = 0;
static uint32_t  s_oom_strikes = 0;

/* Head block: why the cursor currently waits (EVQ_BLOCK_*). Set when claim at
 * the frontier returns ESP_ERR_NOT_FINISHED; cleared when the frontier moves. */
static uint8_t   s_head_block = EVQ_BLOCK_NONE;

/* SD observation (updated under s_mtx by claim + keeper). The epoch bumps on
 * every mount/CID transition and invalidates cached verifications. */
static volatile bool s_sd_parked = false;       /* low-battery guard (app_main) */
static bool      s_sd_last_mounted = false;
static uint32_t  s_sd_last_cid = 0;
static uint32_t  s_sd_epoch = 1;
static volatile uint32_t s_sd_notify_epoch = 0;   /* bumped lock-free by event_log_sd_notify */
static uint32_t  s_sd_notify_seen = 0;
static uint32_t  s_sd_repaired_epoch = 0;       /* epoch whose SD-side repair ran */
static uint8_t   s_sd_state = EVQ_SD_ABSENT;
static bool      s_sd_mismatch_park = false;
static bool      s_sd_full = false;
static TickType_t s_sd_backoff_until = 0;
static uint32_t  s_sd_backoff_ms = 0;
static bool      s_sd_backoff_active = false;

/* Keeper. */
static TaskHandle_t      s_keeper_task = NULL;
static volatile bool     s_keeper_wake_pending = false;
static volatile uint32_t s_keeper_pin_seq = 0;   /* file being copied: exempt from eviction */
static char   *s_kbuf = NULL;                    /* keeper-private I/O buffer (s_line_cap) */

/* One reusable read/claim buffer, allocated once before the log opens. On the
 * normal board malloc(65568) is routed to PSRAM by
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024. A unit whose PSRAM failed to
 * enumerate never attempts that allocation from internal DRAM: it gets only the
 * old 12-KiB buffer and matching runtime store/read caps. Claim, re-claim,
 * tail-id scans, torn-tail repair, quarantine, and OOM archive all run under
 * s_mtx and write at most s_line_cap bytes into this same PSRAM buffer; none of
 * the 64-KiB worst case is task-stack storage. */
static char   *s_line       = NULL;
static size_t  s_line_cap   = 0;
static size_t  s_max_record = 0;

static void evq_keeper_notify(void);
static void evlog_persist_cursor_locked(void);

/* ── small helpers ───────────────────────────────────────────────────── */

static esp_err_t evlog_allocate_line_buffer(void)
{
    if (s_line != NULL) return ESP_OK;       /* event_log_init is normally one-shot */

    bool have_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    size_t cap = have_psram ? EVLOG_LINE_CAP_NORMAL : EVLOG_LINE_CAP_FALLBACK;
    s_line = malloc(cap);                    /* >1 KiB routes to PSRAM when present */
    if (s_line == NULL && have_psram) {
        ESP_LOGE(TAG, "PSRAM event-line allocation (%u B) failed — falling back to degraded %u-B cap",
                 (unsigned)cap, (unsigned)EVLOG_LINE_CAP_FALLBACK);
        cap = EVLOG_LINE_CAP_FALLBACK;
        s_line = malloc(cap);
    }
    if (s_line == NULL) {
        ESP_LOGE(TAG, "event-line allocation failed at degraded %u-B cap",
                 (unsigned)EVLOG_LINE_CAP_FALLBACK);
        return ESP_ERR_NO_MEM;
    }

    s_line_cap = cap;
    s_max_record = cap - EVLOG_RECORD_GUARD_BYTES;
    /* The keeper streams SD copies/imports on its own task without s_mtx, so it
     * gets its own buffer of the same size (a reimported line can be a full
     * record). Absent → the keeper simply does no SD work this session. */
    s_kbuf = malloc(cap);
    if (cap == EVLOG_LINE_CAP_NORMAL) {
        ESP_LOGI(TAG, "event-line buffer: %u B (PSRAM policy), max record %u B",
                 (unsigned)s_line_cap, (unsigned)s_max_record);
    } else {
        ESP_LOGW(TAG, "DEGRADED: PSRAM unavailable; event-line buffer %u B, records >= %u B are refused",
                 (unsigned)s_line_cap, (unsigned)s_max_record);
    }
    return ESP_OK;
}

static void evlog_file_path(char *buf, size_t cap, uint32_t seq)
{
    snprintf(buf, cap, "%s/ev-%06u.log", EVLOG_DIR, (unsigned)seq);
}

static long evlog_file_size(uint32_t seq)
{
    char path[EVQ_PATH_MAX];
    evlog_file_path(path, sizeof path, seq);
    struct stat st;
    return (stat(path, &st) == 0) ? (long)st.st_size : 0;
}

static bool evlog_flash_exists(uint32_t seq, long *size)
{
    char path[EVQ_PATH_MAX];
    evlog_file_path(path, sizeof path, seq);
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (size) *size = (long)st.st_size;
    return true;
}

static void evlog_window_clear_locked(void)
{
    memset(s_window, 0, sizeof s_window);
    s_window_count = 0;
    s_window_bytes = 0;
}

static int evlog_window_find_locked(int64_t measure_id)
{
    for (size_t i = 0; i < s_window_count; i++) {
        if (s_window[i].measure_id == measure_id) return (int)i;
    }
    return -1;
}

static void evlog_window_pop_front_locked(void)
{
    if (s_window_count == 0) return;
    size_t len = (size_t)s_window[0].len;
    if (s_window_count > 1) {
        memmove(&s_window[0], &s_window[1],
                (s_window_count - 1U) * sizeof s_window[0]);
    }
    s_window_count--;
    memset(&s_window[s_window_count], 0, sizeof s_window[0]);
    s_window_bytes = len <= s_window_bytes ? s_window_bytes - len : 0;
}

static bool parse_ev_name(const char *name, uint32_t *seq)
{
    if (strncmp(name, "ev-", 3) != 0) return false;
    const char *p = name + 3;
    if (!isdigit((unsigned char)*p)) return false;
    uint64_t v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) return false;
        p++;
    }
    if (strcmp(p, ".log") != 0) return false;
    *seq = (uint32_t)v;
    return true;
}

/* Scan EVLOG_DIR for the lowest/highest ev-NNNNNN.log seq present. Returns true
 * if any log file exists (then min_seq and max_seq are filled). Caller holds
 * s_mtx (or is single-threaded at init). */
static bool evlog_scan_range_locked(uint32_t *min_seq, uint32_t *max_seq)
{
    uint32_t mn = 0, mx = 0;
    bool any = false;
    DIR *d = opendir(EVLOG_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            uint32_t seq;
            if (parse_ev_name(ent->d_name, &seq)) {
                if (!any) { mn = mx = seq; any = true; }
                else { if (seq < mn) mn = seq; if (seq > mx) mx = seq; }
            }
        }
        closedir(d);
    }
    if (any) { if (min_seq) *min_seq = mn; if (max_seq) *max_seq = mx; }
    return any;
}

/* channel/device/tag/cmd_raw are the only raw fields → strip tab/newline/control
 * so they can't break the line framing; they're short controlled strings anyway. */
static void sanitize_field(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    if (src != NULL) {
        for (size_t i = 0; src[i] != '\0' && j < cap - 1; i++) {
            unsigned char c = (unsigned char)src[i];
            dst[j++] = (c < 0x20 || c == 0x7F) ? '_' : (char)src[i];
        }
    }
    dst[j] = '\0';
}

/* Flush + fsync the tail. Returns ESP_FAIL if the underlying media write failed,
 * so callers can surface it instead of counting torn data as durable. Returns
 * ESP_OK when there is nothing open to flush. */
static esp_err_t evlog_flush_writer_locked(void)
{
    if (s_wf == NULL) return ESP_OK;
    if (fflush(s_wf) != 0 || fsync(fileno(s_wf)) != 0 || ferror(s_wf)) {
        clearerr(s_wf);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Reopen just the tail file for append after a prior open/rotate failure closed it,
 * without the full re-scan of evlog_open_locked. Caller holds s_mtx. */
static esp_err_t evlog_reopen_tail_locked(void)
{
    if (s_wf != NULL) return ESP_OK;
    char path[EVQ_PATH_MAX];
    evlog_file_path(path, sizeof path, s_tail_seq);
    s_wf = fopen(path, "a");
    if (s_wf == NULL) return ESP_FAIL;
    s_tail_size = evlog_file_size(s_tail_seq);
    return ESP_OK;
}

/* ── segment index glue ────────────────────────────────────────────────── */

static bool evq_pos_before_watermark(uint32_t seq, long off)
{
    if (!s_ix.have_watermark) return true;
    if (seq != s_ix.wm_seq) return seq < s_ix.wm_seq;
    return (uint32_t)off < s_ix.wm_off;
}

/* Durable index append (caller holds s_mtx). Failures are logged; every caller
 * treats a failed append as "state did not change" — the RAM fold only moves on
 * success (evq_index_append), so RAM never runs ahead of disk. */
#define EVQ_IX_APPEND(...) evq_index_append(&s_ix, EVQ_INDEX_PATH, __VA_ARGS__)

static void evq_index_maybe_compact_locked(void)
{
    if (!s_ix_ready || s_ix.file_bytes < EVQ_INDEX_COMPACT_BYTES) return;
    if (evq_index_compact(&s_ix, EVQ_INDEX_PATH, EVQ_INDEX_TMP) == ESP_OK) {
        ESP_LOGI(TAG, "segment index compacted: %u line(s), %u B",
                 (unsigned)s_ix.lines, (unsigned)s_ix.file_bytes);
    }
}

/* Register a just-rotated (closed) file with its running statistics. */
static void evq_register_rotated_locked(uint32_t seq, int64_t first, int64_t last,
                                        uint32_t count, uint32_t bytes, uint32_t crc)
{
    if (!s_ix_ready || count == 0) return;
    esp_err_t err = EVQ_IX_APPEND("S %" PRIu32 " %lld %lld %" PRIu32 " %" PRIu32 " %08" PRIx32,
                                  seq, (long long)first, (long long)last, count, bytes, crc);
    if (err == ESP_OK) {
        evq_seg_t *s = evq_index_find(&s_ix, seq);
        if (s) s->flash_present = true;
    } else {
        /* Stays unindexed: flash-only, never spooled or reclaimed until the
         * keeper indexes it by scanning. */
        s_unidx_present = true;
        ESP_LOGW(TAG, "index S line for ev-%06u.log failed (%s) — keeper will index it",
                 (unsigned)seq, esp_err_to_name(err));
    }
}

/* ── SD observation ─────────────────────────────────────────────────────── */

/* Refresh the SD view (no FS operation: mount flag, CID, loss latch, park flag
 * only — a mismatched card must receive ZERO operations, and deciding that must
 * not itself touch the card). Caller holds s_mtx. */
static void evq_sd_observe_locked(void)
{
    bool mounted = sdcard_is_mounted();
    uint32_t cid = mounted ? sdcard_card_serial() : 0;
    uint32_t ne = s_sd_notify_epoch;
    if (mounted != s_sd_last_mounted || cid != s_sd_last_cid || ne != s_sd_notify_seen) {
        s_sd_notify_seen = ne;
        s_sd_epoch++;
        s_sd_last_mounted = mounted;
        s_sd_last_cid = cid;
        s_sd_full = false;
        s_sd_backoff_active = false;
        s_sd_backoff_ms = 0;
        if (mounted) evq_keeper_notify();
    }

    /* A card is parked (zero operations) when any obligation that only an SD
     * copy can satisfy — SD_ONLY or REIMPORT — lives on another CID. */
    bool park = false;
    if (mounted) {
        for (size_t i = 0; i < s_ix.n; i++) {
            const evq_seg_t *s = &s_ix.segs[i];
            if ((s->state == EVQ_SEG_SD_ONLY || s->state == EVQ_SEG_REIMPORT ||
                 (s->state == EVQ_SEG_DELIVERED && !s->flash_present && s->seq >= s_rd_seq)) &&
                s->cid != 0 && s->cid != cid) {
                park = true;
                break;
            }
        }
    }
    s_sd_mismatch_park = park;

    if (s_sd_parked)            s_sd_state = EVQ_SD_PARKED;
    else if (!mounted)          s_sd_state = sdcard_io_lost() ? EVQ_SD_LOST : EVQ_SD_ABSENT;
    else if (sdcard_io_lost())  s_sd_state = EVQ_SD_LOST;
    else if (park)              s_sd_state = EVQ_SD_MISMATCH;
    else if (s_sd_full)         s_sd_state = EVQ_SD_FULL;
    else                        s_sd_state = EVQ_SD_OK;
}

static bool evq_sd_usable_locked(void)
{
    evq_sd_observe_locked();
    return s_sd_state == EVQ_SD_OK || s_sd_state == EVQ_SD_FULL;
}

/* ── SD file helpers (keeper + claim) ──────────────────────────────────── */

typedef struct {
    uint32_t crc;
    uint32_t bytes;
    uint32_t count;
    int64_t  first_id;
    int64_t  last_id;
    bool     framed;     /* every line newline-terminated (last byte '\n') */
    bool     io_error;
} evq_scan_t;

/* Stream a file computing size, CRC and line structure. `buf` is scratch of
 * `cap` bytes. Needs no line buffer: ids are parsed from the first digits after
 * each newline (records begin with their measure_id). */
static bool evq_scan_file(const char *path, char *buf, size_t cap, evq_scan_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    uint32_t crc = 0;
    bool at_line_start = true, parsing_id = false;
    int64_t cur_id = 0;
    char last = '\n';
    size_t n;
    while ((n = fread(buf, 1, cap, f)) > 0) {
        crc = evq_crc32(crc, buf, n);
        out->bytes += (uint32_t)n;
        for (size_t i = 0; i < n; i++) {
            char c = buf[i];
            if (at_line_start) {
                at_line_start = false;
                parsing_id = true;
                cur_id = 0;
            }
            if (parsing_id) {
                if (c >= '0' && c <= '9') cur_id = cur_id * 10 + (c - '0');
                else parsing_id = false;
            }
            if (c == '\n') {
                out->count++;
                if (out->count == 1) out->first_id = cur_id;
                out->last_id = cur_id;
                at_line_start = true;
                parsing_id = false;
            }
        }
        last = buf[n - 1];
    }
    out->io_error = ferror(f) != 0;
    fclose(f);
    out->crc = crc;
    out->framed = (out->bytes == 0) || last == '\n';
    return !out->io_error;
}

static bool evq_scan_matches(const evq_scan_t *sc, const evq_seg_t *s)
{
    return sc->framed && !sc->io_error && sc->bytes == s->bytes && sc->crc == s->crc &&
           sc->count == s->count && sc->first_id == s->first_id && sc->last_id == s->last_id;
}

static void evq_sd_primary_path(char *buf, size_t cap, const char *name)
{
    snprintf(buf, cap, "%s/%s", EVLOG_LEGACY_SD_DIR, name);
}
static void evq_sd_mirror_path(char *buf, size_t cap, const char *name)
{
    snprintf(buf, cap, "%s/%s", EVQ_MIRROR_DIR, name);
}

static void evq_sd_backoff_locked(void)
{
    s_spool_errors++;
    s_sd_backoff_ms = s_sd_backoff_ms == 0 ? EVQ_SD_BACKOFF_MIN_MS
                    : (s_sd_backoff_ms * 2 > EVQ_SD_BACKOFF_MAX_MS ? EVQ_SD_BACKOFF_MAX_MS : s_sd_backoff_ms * 2);
    s_sd_backoff_until = xTaskGetTickCount() + pdMS_TO_TICKS(s_sd_backoff_ms);
    s_sd_backoff_active = true;
    ESP_LOGW(TAG, "SD transfer error — backing off %u ms", (unsigned)s_sd_backoff_ms);
}

static bool evq_sd_backoff_elapsed(void)
{
    if (!s_sd_backoff_active) return true;
    return (int32_t)(xTaskGetTickCount() - s_sd_backoff_until) >= 0;
}

/* ── read-source abstraction (claim / quarantine / OOM archive) ─────────── */

typedef struct {
    FILE   *f;
    bool    sd_ref;
    uint8_t medium;
} evq_rd_t;

static void evq_rd_close(evq_rd_t *rd)
{
    if (rd->f != NULL) fclose(rd->f);
    if (rd->sd_ref) sdcard_io_end();
    rd->f = NULL;
    rd->sd_ref = false;
}

/* Move a damaged SD copy of segment `seq` aside to /sdcard/evq/bad-<seq>-<k>.log
 * (contract amendment A1): the bytes are kept for inspection, never deleted,
 * but leave /sdcard/events (a rolled-back firmware would import them verbatim)
 * and the m-* mirror names (orphan-mirror import). Caller holds an SD ref.
 * A rename touches only directory entries, so it works on a copy whose data
 * clusters no longer read back. */
static bool evq_sd_move_aside(const char *path, uint32_t seq)
{
    char bad[EVQ_PATH_MAX];
    struct stat bst;
    unsigned k = 0;
    for (; k < 100; k++) {
        snprintf(bad, sizeof bad, "%s/bad-%06u-%u.log", EVQ_MIRROR_DIR, (unsigned)seq, k);
        if (stat(bad, &bst) != 0) break;
    }
    if (k == 100 || rename(path, bad) != 0) return false;
    s_sd_bad_copies++;
    ESP_LOGW(TAG, "damaged copy %s moved aside to %s", path, bad);
    return true;
}

/* Verify one SD copy of `s` in place (caller holds s_mtx AND an SD ref). */
static bool evq_sd_verify_copy_locked(const evq_seg_t *s, bool mirror)
{
    char path[EVQ_PATH_MAX];
    if (mirror) evq_sd_mirror_path(path, sizeof path, s->mirror);
    else        evq_sd_primary_path(path, sizeof path, s->primary);
    evq_scan_t sc;
    if (!evq_scan_file(path, s_line, s_line_cap, &sc)) return false;
    return evq_scan_matches(&sc, s);
}

/* Open a verified SD copy of `s` for reading. Holds an SD ref on success.
 * On failure sets *block to the reason and returns ESP_ERR_NOT_FINISHED. */
static esp_err_t evq_sd_open_verified_locked(evq_seg_t *s, evq_rd_t *rd, uint8_t *block)
{
    if (!evq_sd_usable_locked()) {
        *block = (s_sd_state == EVQ_SD_PARKED)   ? EVQ_BLOCK_SD_PARKED
               : (s_sd_state == EVQ_SD_MISMATCH) ? EVQ_BLOCK_SD_MISMATCH
               : (s_sd_state == EVQ_SD_LOST)     ? EVQ_BLOCK_SD_LOST : EVQ_BLOCK_SD_ABSENT;
        return ESP_ERR_NOT_FINISHED;
    }
    if (s->cid != s_sd_last_cid) { *block = EVQ_BLOCK_SD_MISMATCH; return ESP_ERR_NOT_FINISHED; }
    if (!sdcard_io_begin()) { *block = EVQ_BLOCK_SD_LOST; return ESP_ERR_NOT_FINISHED; }

    if (s->sd_verified_epoch != s_sd_epoch) {
        EVQ_FAULT_POINT("claim.sd_read_error");
        uint8_t which = 0;
        if (s->primary[0] != '\0' && evq_sd_verify_copy_locked(s, false)) {
            which = 1;
        } else if (s->mirror[0] != '\0' && evq_sd_verify_copy_locked(s, true)) {
            which = 2;
            s_mirror_used++;
            ESP_LOGW(TAG, "ev-%06u.log: SD primary failed verification — using mirror", (unsigned)s->seq);
            /* The damaged primary is kept for inspection (bytes untouched) but
             * moved out of /sdcard/events, where a rolled-back firmware would
             * import it verbatim. */
            char pp[EVQ_PATH_MAX];
            evq_sd_primary_path(pp, sizeof pp, s->primary);
            struct stat bst;
            if (stat(pp, &bst) == 0) (void)evq_sd_move_aside(pp, s->seq);
        }
        if (which == 0) {
            char pp[EVQ_PATH_MAX], mp[EVQ_PATH_MAX];
            evq_sd_primary_path(pp, sizeof pp, s->primary);
            evq_sd_mirror_path(mp, sizeof mp, s->mirror);
            struct stat st;
            bool any = stat(pp, &st) == 0 || stat(mp, &st) == 0;
            sdcard_io_end();
            *block = any ? EVQ_BLOCK_BACKLOG_CORRUPT : EVQ_BLOCK_BACKLOG_MISSING;
            if (any) s_corrupt_medium = EVQ_MEDIUM_SD;
            return ESP_ERR_NOT_FINISHED;
        }
        s->sd_verified_epoch = s_sd_epoch;
        s->sd_verified_which = which;
    }

    char path[EVQ_PATH_MAX];
    if (s->sd_verified_which == 2) evq_sd_mirror_path(path, sizeof path, s->mirror);
    else                           evq_sd_primary_path(path, sizeof path, s->primary);
    EVQ_FAULT_POINT("claim.sd_short_read");
    rd->f = fopen(path, "rb");
    if (rd->f == NULL) {
        sdcard_io_end();
        s->sd_verified_epoch = 0;             /* re-verify next time */
        *block = EVQ_BLOCK_SD_LOST;
        sdcard_report_io_error();
        return ESP_ERR_NOT_FINISHED;
    }
    rd->sd_ref = true;
    rd->medium = EVQ_MEDIUM_SD;
    return ESP_OK;
}

/* Verify a rotated flash file's CRC once per boot (G8: post-commit flash
 * corruption must be detected, not delivered). Caller holds s_mtx. */
static bool evq_flash_crc_ok_locked(evq_seg_t *s)
{
    if (s->flash_crc_checked == 0) {
        char path[EVQ_PATH_MAX];
        evlog_file_path(path, sizeof path, s->seq);
        evq_scan_t sc;
        bool ok = evq_scan_file(path, s_line, s_line_cap, &sc) && evq_scan_matches(&sc, s);
        s->flash_crc_checked = ok ? 1 : 2;
        if (!ok) {
            s_corrupt_detected += s->count;
            ESP_LOGE(TAG, "ev-%06u.log: flash copy fails its index CRC — never delivered from flash",
                     (unsigned)s->seq);
        }
    }
    return s->flash_crc_checked == 1;
}

/* Open queue file `seq` for reading from whichever verified copy exists.
 *   ESP_OK               rd open (flash or verified SD)
 *   ESP_ERR_NOT_FINISHED an indexed obligation that cannot be read now (*block)
 *   ESP_ERR_NOT_FOUND    no copy anywhere and not indexed (pre-upgrade gap)
 *   ESP_FAIL             flash I/O error
 * Caller holds s_mtx. */
static esp_err_t evq_open_read_locked(uint32_t seq, evq_rd_t *rd, uint8_t *block)
{
    memset(rd, 0, sizeof *rd);
    *block = EVQ_BLOCK_NONE;
    char path[EVQ_PATH_MAX];
    evlog_file_path(path, sizeof path, seq);
    evq_seg_t *s = (seq < s_tail_seq) ? evq_index_find(&s_ix, seq) : NULL;

    errno = 0;
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        if (s != NULL && !evq_flash_crc_ok_locked(s)) {
            fclose(f);
            if (s->primary[0] != '\0') {
                esp_err_t e = evq_sd_open_verified_locked(s, rd, block);
                if (e == ESP_OK) return ESP_OK;
                if (*block == EVQ_BLOCK_BACKLOG_CORRUPT || *block == EVQ_BLOCK_BACKLOG_MISSING) {
                    s_corrupt_medium = EVQ_MEDIUM_FLASH;
                    *block = EVQ_BLOCK_BACKLOG_CORRUPT;
                }
                return e;
            }
            s_corrupt_medium = EVQ_MEDIUM_FLASH;
            *block = EVQ_BLOCK_BACKLOG_CORRUPT;
            return ESP_ERR_NOT_FINISHED;
        }
        rd->f = f;
        rd->medium = EVQ_MEDIUM_FLASH;
        return ESP_OK;
    }
    if (errno != ENOENT) return ESP_FAIL;
    if (seq >= s_tail_seq) return ESP_ERR_NOT_FOUND;
    if (s == NULL) return ESP_ERR_NOT_FOUND;
    if (s->flash_present) s->flash_present = false;
    if (s->primary[0] == '\0' || s->state == EVQ_SEG_FLASH || s->state == EVQ_SEG_REIMPORT) {
        *block = EVQ_BLOCK_BACKLOG_MISSING;         /* indexed, no SD copy: wait, never skip */
        return ESP_ERR_NOT_FINISHED;
    }
    return evq_sd_open_verified_locked(s, rd, block);
}

/* Size of queue file `seq` from whichever copy defines it (flash stat, else
 * the index). false = unknown (cursor must not advance past it). */
static bool evq_file_size_locked(uint32_t seq, long *size)
{
    if (evlog_flash_exists(seq, size)) return true;
    evq_seg_t *s = evq_index_find(&s_ix, seq);
    if (s != NULL && s->primary[0] != '\0' &&
        (s->state == EVQ_SEG_SPOOLED || s->state == EVQ_SEG_SD_ONLY || s->state == EVQ_SEG_DELIVERED)) {
        *size = (long)s->bytes;
        return true;
    }
    return false;
}

/* ── quarantine (intact copy BEFORE the cursor moves) ──────────────────── */

typedef enum { EVQ_Q_POISON, EVQ_Q_MALFORMED } evq_qclass_t;

/* Copy `len` raw bytes at `off` of queue file `seq` to quarantine.log and fsync.
 * true only once the copy is durable; the caller moves the cursor only then.
 * Uses s_line as a chunk buffer. Caller holds s_mtx. */
static bool evq_quarantine_range_locked(uint32_t seq, long off, long len)
{
    if (len <= 0) return false;
    evq_rd_t rd;
    uint8_t block;
    if (evq_open_read_locked(seq, &rd, &block) != ESP_OK) return false;
    EVQ_FAULT_POINT("quarantine.before_copy");
    bool ok = fseek(rd.f, off, SEEK_SET) == 0;
    FILE *qf = ok ? fopen(EVLOG_QUARANTINE, "a") : NULL;
    ok = ok && qf != NULL;
    long left = len;
    while (ok && left > 0) {
        size_t want = (size_t)left < s_line_cap ? (size_t)left : s_line_cap;
        size_t got = fread(s_line, 1, want, rd.f);
        if (got == 0) { ok = false; break; }
        if (fwrite(s_line, 1, got, qf) != got) { ok = false; break; }
        left -= (long)got;
    }
    if (qf != NULL) {
        if (ok && (fflush(qf) != 0 || fsync(fileno(qf)) != 0)) ok = false;
        if (fclose(qf) != 0) ok = false;
    }
    evq_rd_close(&rd);
    EVQ_FAULT_POINT("quarantine.after_copy_before_cursor");
    return ok;
}

/* Account one record passed at the cursor WITHOUT an ACK, after its intact copy
 * is durable in quarantine. Classification: bytes this firmware wrote (at/after
 * the upgrade watermark) can only be malformed through post-commit corruption. */
static void evq_account_quarantined_locked(evq_qclass_t cls, uint32_t seq, long off)
{
    s_skipped++;
    if (cls == EVQ_Q_POISON) s_quarantined_poison++;
    else if (evq_pos_before_watermark(seq, off)) s_quarantined_malformed++;
    else { s_corrupt_detected++; s_corrupt_medium = EVQ_MEDIUM_FLASH; }
}

/* ── cursor persistence ─────────────────────────────────────────────────── */

static uint32_t evq_blob_crc(uint32_t seq, uint32_t off)
{
    uint32_t v[2] = { seq, off };
    return evq_crc32(0x5EC0, v, sizeof v);
}

/* Persist the cursor: the atomic blob FIRST (only this firmware writes it), then
 * the legacy keys that older firmware reads. A tear between the two leaves the
 * legacy value BEHIND the blob, which boot treats as "use the earlier value"
 * (duplicates, never a skip); legacy AHEAD of the blob means another firmware
 * moved the cursor (a rollback) — see evq_reconcile_cursor_locked. */
static void evlog_persist_cursor_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    evlog_cursor_blob_t b = { .seq = s_rd_seq, .off = (uint32_t)s_rd_off };
    b.crc = evq_blob_crc(b.seq, b.off);
    nvs_set_blob(h, NVS_KEY_CUR, &b, sizeof b);
    nvs_commit(h);
    EVQ_FAULT_POINT("cursor.after_blob_before_legacy");
    nvs_set_u32(h, NVS_KEY_RD_SEQ, s_rd_seq);
    nvs_set_u32(h, NVS_KEY_RD_OFF, (uint32_t)s_rd_off);
    nvs_commit(h);
    nvs_close(h);
}

/* The cursor just passed EOF of `seq` by this firmware's own ACK prefix (or a
 * frontier quarantine). Persist, then mark the file DELIVERED in the index. */
static void evq_cursor_leave_file_locked(void)
{
    uint32_t done = s_rd_seq;
    s_rd_seq++;
    s_rd_off = 0;
    s_cur_consumed = 0;
    s_cur_validated = true;
    evlog_persist_cursor_locked();
    EVQ_FAULT_POINT("cursor.after_nvs_before_index_delivered");
    evq_seg_t *s = evq_index_find(&s_ix, done);
    if (s != NULL && s->state != EVQ_SEG_DELIVERED && s->state != EVQ_SEG_REIMPORT) {
        if (EVQ_IX_APPEND("D %" PRIu32, done) != ESP_OK) {
            /* Boot repair re-derives DELIVERED from the blob. */
        }
    }
}

/* Step over a rotated file only when the durable cursor itself has reached its
 * EOF.  The claim tail may already be in a later file, but it is deliberately
 * irrelevant here: a reboot must still find every un-ACKed byte at or after the
 * cursor. Drained files are NOT deleted here — the keeper archives them to SD
 * (or eviction reclaims their flash copy); unsynced data always outranks synced
 * copies. The file size comes from flash, else from the index (SD-only). */
static void evlog_normalize_cursor_locked(void)
{
    while (s_rd_seq < s_tail_seq) {
        long size = 0;
        /* An unknown size is not proof of EOF. Leave the cursor so the claim
         * path can distinguish a genuinely missing file from an I/O failure. */
        if (!evq_file_size_locked(s_rd_seq, &size) || s_rd_off < size) break;
        evq_cursor_leave_file_locked();
    }
}

/* ── synced-first eviction (audit C1 successor) ────────────────────────────
 * When the internal store runs short, free space by deleting the OLDEST fully
 * DELIVERED flash copies (seq < s_rd_seq strictly — the cursor file may hold
 * unsynced bytes and is never touched, nor is anything after it). What is lost
 * is only the not-yet-archived SD copy of data the platform already ACKed; for
 * a spooled file the SD copies remain and are archived later. Returns true if at
 * least one file was evicted. Caller holds s_mtx. */
static int64_t s_evicted_files = 0;   /* lifetime counter (boot-relative), logged */
static bool evlog_evict_to_locked(uint64_t target)
{
    bool evicted = false;
    uint64_t freeb = 0;
    uint32_t scan_from = 0;
    while (evstore_free_bytes(&freeb) == ESP_OK && freeb < target) {
        /* Oldest flash file strictly behind the cursor that is NOT still owed:
         * a REIMPORT entry's flash copy may be its only copy (the cursor moved
         * without our ACK), so it is never an eviction candidate. */
        uint32_t min_seq = 0, max_seq = 0;
        if (!evlog_scan_range_locked(&min_seq, &max_seq)) break;
        uint32_t cand = 0;
        for (uint32_t q = (min_seq > scan_from ? min_seq : scan_from); q < s_rd_seq && q <= max_seq; q++) {
            if (!evlog_flash_exists(q, NULL)) continue;
            const evq_seg_t *e = evq_index_find(&s_ix, q);
            if (e != NULL && e->state == EVQ_SEG_REIMPORT) continue;
            if (q == s_keeper_pin_seq) continue;      /* keeper is copying it right now */
            cand = q;
            break;
        }
        if (cand == 0) break;                        /* nothing evictable left */
        min_seq = cand;
        scan_from = cand + 1;
        char path[EVQ_PATH_MAX];
        evlog_file_path(path, sizeof path, min_seq);
        if (remove(path) != 0) break;                /* stuck file: don't spin */
        evicted = true;
        s_evicted_files++;
        evq_seg_t *s = evq_index_find(&s_ix, min_seq);
        if (s != NULL) {
            s->flash_present = false;
            if (s->primary[0] == '\0') (void)EVQ_IX_APPEND("A %" PRIu32, min_seq);   /* delivered, no SD copy left to archive */
        }
        ESP_LOGW(TAG, "evicted delivered %s to protect unsent capacity "
                      "(%llu B free, %lld evicted so far)",
                 path, (unsigned long long)freeb, (long long)s_evicted_files);
    }
    return evicted;
}

static bool evlog_evict_synced_locked(void)
{
    return evlog_evict_to_locked(EVLOG_EVICT_TARGET);
}

/* Full-store recovery, shared by EVERY ENOSPC-class failure path: evict the
 * oldest delivered flash copies, and wake the keeper to spool + reclaim, before
 * anything is dropped or reported as a media fault. Returns true when the store
 * has headroom again. On false, s_write_full is latched so later stores fail
 * fast at the admission check. Caller holds s_mtx. */
static bool evlog_full_recovery_locked(void)
{
    uint64_t freeb = 0;
    if (evstore_free_bytes(&freeb) == ESP_OK && freeb < EVLOG_MIN_FREE_BYTES) {
        (void)evlog_evict_synced_locked();
    }
    if (evstore_free_bytes(&freeb) == ESP_OK && freeb >= EVLOG_MIN_FREE_BYTES) {
        return true;
    }
    if (!s_write_full) {
        ESP_LOGW(TAG, "store full (%llu B free) — writes paused until spool/reclaim or the drain frees space",
                 (unsigned long long)freeb);
    }
    s_write_full = true;
    evq_keeper_notify();
    return false;
}

/* Advance only the contiguous ACKed prefix.  Out-of-order ACKs merely flip a
 * slot flag; neither the RAM cursor nor NVS can cross the first unacked slot.
 * The batched persistence rule is unchanged: an unpersisted RAM advance can
 * replay duplicates after reboot, but the stored cursor can never skip data. */
static void evlog_advance_acked_prefix_locked(void)
{
    while (s_window_count > 0 && s_window[0].acked) {
        evlog_window_slot_t front = s_window[0];
        if (front.seq != s_rd_seq || front.off != s_rd_off) {
            ESP_LOGE(TAG, "window frontier mismatch: cursor=%u:%ld slot=%u:%ld id=%lld — RESETTING volatile window; durable cursor will replay",
                     (unsigned)s_rd_seq, s_rd_off, (unsigned)front.seq, front.off,
                     (long long)front.measure_id);
            /* The cursor is durable truth. Holding this impossible RAM shape
             * forever turns one invariant violation into a permanent publisher
             * outage; abandoning claims costs only bounded duplicates. */
            evlog_window_clear_locked();
            s_reset_pending = true;
            return;
        }

        s_rd_seq = front.seq;
        s_rd_off = front.off + front.len;
        s_cur_consumed++;
        s_last_acked_id = front.measure_id;
        evlog_window_pop_front_locked();

        if (++s_acks_since_persist >= EVLOG_CURSOR_BATCH) {
            EVQ_FAULT_POINT("ack.after_ram_prefix_before_nvs");
            evlog_persist_cursor_locked();
            s_acks_since_persist = 0;
        }
        evlog_normalize_cursor_locked();
    }
}

static void evlog_persist_nid_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u64(h, NVS_KEY_NID, (uint64_t)s_id_limit);
    nvs_commit(h);
    nvs_close(h);
}

static void evlog_bump_next_id_locked(int64_t seen_id)
{
    if (seen_id + 1 > s_next_id) {
        s_next_id = seen_id + 1;
        if (s_next_id >= s_id_limit) {
            s_id_limit = s_next_id + EVLOG_ID_BLOCK;
            evlog_persist_nid_locked();
        }
    }
}

/* ── exact pending accounting ───────────────────────────────────────────── */

typedef struct {
    int64_t pending, flash, sd, reimport;
    bool    exact;
} evq_counts_t;

/* Pending = records at/after the cursor on either medium + records owed by
 * REIMPORT entries. Exact from the index counts, the cursor file's consumed
 * count and the tail counter; a FLOOR while unindexed files, an unvalidated
 * cursor offset, or an unknown reimport remainder exist. Caller holds s_mtx. */
static void evq_counts_locked(evq_counts_t *c)
{
    memset(c, 0, sizeof *c);
    c->exact = s_ix_ready && !s_unidx_present && s_cur_validated;
    for (size_t i = 0; i < s_ix.n; i++) {
        const evq_seg_t *s = &s_ix.segs[i];
        if (s->state == EVQ_SEG_REIMPORT) {
            if (s->reimport_remaining < 0) { c->exact = false; c->reimport += s->count; }
            else c->reimport += s->reimport_remaining;
            continue;
        }
        if (s->seq < s_rd_seq || s->seq >= s_tail_seq) continue;
        int64_t n = s->count;
        if (s->seq == s_rd_seq) n = (int64_t)s->count > (int64_t)s_cur_consumed ? (int64_t)s->count - s_cur_consumed : 0;
        bool on_sd_only = !s->flash_present && s->primary[0] != '\0';
        if (on_sd_only) c->sd += n; else c->flash += n;
    }
    if (s_tail_seq >= s_rd_seq) {
        int64_t n = s_tail_count;
        if (s_tail_seq == s_rd_seq) n = (int64_t)s_tail_count > (int64_t)s_cur_consumed ? (int64_t)s_tail_count - s_cur_consumed : 0;
        c->flash += n;
    }
    if (s_unidx_present) c->flash += s_unidx_floor;
    c->reimport += s_legacy_pending;
    if (!s_legacy_known) c->exact = false;
    c->pending = c->flash + c->sd + c->reimport;
}

/* Parse one (newline-included) record line in place. Returns:
 *   ESP_OK                  — out filled (heap metadata/payload; free with measurement_event_free)
 *   ESP_ERR_INVALID_RESPONSE — malformed; caller quarantines it at the frontier
 *   ESP_ERR_NO_MEM          — alloc failed; caller must NOT consume the record */
static esp_err_t parse_record(char *line, size_t len, measurement_event_t *out)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

    char *f[9];
    f[0] = line;
    int nf = 1;
    for (char *p = line; *p != '\0' && nf < 9; p++) {
        if (*p == '\t') { *p = '\0'; f[nf++] = p + 1; }
    }
    if (nf < 9) return ESP_ERR_INVALID_RESPONSE;        /* need exactly 9 fields (v2) */

    const char *payload = f[8];
    if (payload[0] != '{' && payload[0] != '[') return ESP_ERR_INVALID_RESPONSE;

    memset(out, 0, sizeof *out);
    out->measure_id     = (int64_t)strtoll(f[0], NULL, 10);
    strncpy(out->channel, f[1], sizeof(out->channel) - 1);
    strncpy(out->device,  f[2], sizeof(out->device)  - 1);
    strncpy(out->tag,     f[3], sizeof(out->tag)     - 1);
    out->start_ticks_ms = (int64_t)strtoll(f[5], NULL, 10);
    out->end_ticks_ms   = (int64_t)strtoll(f[6], NULL, 10);
    out->sync_state     = MEASUREMENT_SYNC_INFLIGHT;

    /* cmd_raw is variable-length (a full multi-segment "arrun …" command) → heap.
     * NULL when the field is empty. */
    if (f[4][0] != '\0') {
        out->cmd_raw = strdup(f[4]);
        if (out->cmd_raw == NULL) return ESP_ERR_NO_MEM;
    }
    if (f[7][0] != '\0') {
        out->metadata_json = strdup(f[7]);
        if (out->metadata_json == NULL) { measurement_event_free(out); return ESP_ERR_NO_MEM; }
    }
    out->payload_json = strdup(payload);
    if (out->payload_json == NULL) {
        measurement_event_free(out);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Structural validity of a framed v2 line WITHOUT allocating: exactly what
 * parse_record accepts. Imports route anything else to quarantine. */
static bool evq_line_is_valid(const char *line, size_t len)
{
    if (len == 0 || line[len - 1] != '\n') return false;
    int tabs = 0;
    const char *payload = NULL;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '\t') { tabs++; if (tabs == 8) payload = line + i + 1; }
    }
    if (tabs < 8 || payload == NULL) return false;
    if (payload[0] != '{' && payload[0] != '[') return false;
    return strtoll(line, NULL, 10) > 0;
}

/* ── boot ───────────────────────────────────────────────────────────────── */

/* Scan the tail file: repair a brownout-torn final record, then derive the
 * running segment statistics (count, CRC, first/last id). Caller holds s_mtx /
 * is init. Uses s_line. */
static void evlog_scan_tail_locked(void)
{
    char path[EVQ_PATH_MAX];
    evlog_file_path(path, sizeof path, s_tail_seq);

    /* Repair a brownout-torn final record: an ungraceful reset can leave bytes
     * after the last fsync (the record was never acknowledged). Truncate back to
     * the last newline so the next append can't concatenate into — and corrupt
     * the framing of — the torn record (audit B3). Accepted records are fsync'd
     * before ESP_OK, so this never removes one. */
    long tsz = evlog_file_size(s_tail_seq);
    if (tsz > 0) {
        FILE *tf = fopen(path, "rb+");
        if (tf != NULL) {
            long scan = (tsz < (long)s_line_cap) ? tsz : (long)s_line_cap;
            if (fseek(tf, tsz - scan, SEEK_SET) == 0) {
                size_t got = fread(s_line, 1, (size_t)scan, tf);
                long last_nl = -1;
                for (long i = (long)got - 1; i >= 0; i--) {
                    if (s_line[i] == '\n') { last_nl = i; break; }
                }
                if (last_nl >= 0) {
                    long good_end = tsz - scan + last_nl + 1;
                    if (good_end < tsz && ftruncate(fileno(tf), good_end) == 0) {
                        fsync(fileno(tf));
                        ESP_LOGW(TAG, "repaired torn tail %s: %ld -> %ld B", path, tsz, good_end);
                    }
                } else if (tsz < (long)s_line_cap && ftruncate(fileno(tf), 0) == 0) {
                    fsync(fileno(tf));      /* one torn fragment and nothing else */
                    ESP_LOGW(TAG, "repaired torn tail %s: %ld -> 0 B", path, tsz);
                }
            }
            fclose(tf);
        }
    }

    evq_scan_t sc;
    if (evq_scan_file(path, s_line, s_line_cap, &sc)) {
        s_tail_count = sc.count;
        s_tail_crc = sc.crc;
        s_tail_first_id = sc.first_id;
        s_tail_last_id = sc.last_id;
    } else {
        s_tail_count = 0; s_tail_crc = 0; s_tail_first_id = s_tail_last_id = 0;
    }
}

/* Validate the cursor offset against the record framing and count the lines
 * before it (s_cur_consumed). An offset that is not a record boundary (a torn
 * two-key NVS write from older firmware) falls back to the file start: bounded
 * duplicates, never a misaligned parse that "skips" a record. Needs the file
 * readable (flash or a verified SD copy); otherwise stays unvalidated. */
static void evq_validate_cursor_locked(void)
{
    if (s_cur_validated) return;
    if (s_rd_off == 0) { s_cur_consumed = 0; s_cur_validated = true; return; }
    evq_rd_t rd;
    uint8_t block;
    if (evq_open_read_locked(s_rd_seq, &rd, &block) != ESP_OK) return;
    uint32_t lines = 0;
    long left = s_rd_off;
    char last = '\n';
    bool ok = true;
    while (left > 0) {
        size_t want = (size_t)left < s_line_cap ? (size_t)left : s_line_cap;
        size_t got = fread(s_line, 1, want, rd.f);
        if (got == 0) { ok = false; break; }
        for (size_t i = 0; i < got; i++) if (s_line[i] == '\n') lines++;
        last = s_line[got - 1];
        left -= (long)got;
    }
    evq_rd_close(&rd);
    if (!ok) return;                           /* I/O: retry on the next claim */
    if (last != '\n') {
        ESP_LOGW(TAG, "cursor off=%ld in ev-%06u.log is not a record boundary — replaying file from 0",
                 s_rd_off, (unsigned)s_rd_seq);
        s_rd_off = 0;
        s_cur_consumed = 0;
        evlog_persist_cursor_locked();
    } else {
        s_cur_consumed = lines;
    }
    s_cur_validated = true;
}

/* Reconcile the atomic blob with the legacy keys (see evlog_persist_cursor_locked).
 * Returns the cursor to use. `foreign` = another firmware advanced it. */
static void evq_reconcile_cursor_locked(uint32_t *seq, uint32_t *off, bool *foreign,
                                        uint32_t *blob_seq, uint32_t *blob_off)
{
    uint32_t lseq = 0, loff = 0;
    bool have_legacy = false;
    evlog_cursor_blob_t b = {0};
    bool have_blob = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        have_legacy = nvs_get_u32(h, NVS_KEY_RD_SEQ, &lseq) == ESP_OK;
        if (nvs_get_u32(h, NVS_KEY_RD_OFF, &loff) != ESP_OK) loff = 0;
        size_t len = sizeof b;
        have_blob = nvs_get_blob(h, NVS_KEY_CUR, &b, &len) == ESP_OK && len == sizeof b &&
                    b.crc == evq_blob_crc(b.seq, b.off);
        nvs_close(h);
    }
    *foreign = false;
    *blob_seq = b.seq; *blob_off = b.off;
    if (!have_blob) {                          /* first boot of this firmware */
        *seq = have_legacy ? lseq : 0;
        *off = have_legacy ? loff : 0;
        return;
    }
    if (!have_legacy) { *seq = b.seq; *off = b.off; return; }
    bool legacy_ahead = (lseq > b.seq) || (lseq == b.seq && loff > b.off);
    bool legacy_behind = (lseq < b.seq) || (lseq == b.seq && loff < b.off);
    if (legacy_ahead) {
        *foreign = true;                       /* a rollback moved it */
        *seq = lseq; *off = loff;
    } else if (legacy_behind) {
        *seq = lseq; *off = loff;              /* our own torn write: earlier = duplicates only */
    } else {
        *seq = b.seq; *off = b.off;
    }
}

/* Mark every SPOOLED/SD_ONLY file the cursor passed WITHOUT our ACK as REIMPORT.
 * A cursor move by another firmware is never taken as proof of delivery. */
static void evq_mark_foreign_range_locked(uint32_t from_seq, uint32_t from_off, uint32_t to_seq)
{
    /* Walk EVERY indexed segment in the range in seq order (no fixed-size
     * batch: a segment left out here would be taken as delivered and archived).
     * Appends may remove entries, so re-locate the next one by seq each time. */
    uint32_t next = from_seq;
    for (;;) {
        evq_seg_t *s = NULL;
        for (size_t i = 0; i < s_ix.n; i++) {
            evq_seg_t *c = &s_ix.segs[i];
            if (c->seq >= next && c->seq < to_seq && c->state != EVQ_SEG_REIMPORT) { s = c; break; }
        }
        if (s == NULL) break;
        next = s->seq + 1;
        s->flash_present = evlog_flash_exists(s->seq, NULL);
        if (!s->flash_present && s->primary[0] == '\0') {
            /* Flash-only and gone: the other firmware can only have removed
             * it at/behind ITS cursor after reading it. Retire the entry. */
            (void)EVQ_IX_APPEND("A %" PRIu32, s->seq);
            continue;
        }
        /* Even a flash-resident file is re-imported: a cursor move by other
         * firmware is not proof of delivery (duplicates, never a loss). */
        uint32_t off = (s->seq == from_seq) ? from_off : 0;
        int64_t rem = off == 0 ? (int64_t)s->count : -1;
        if (EVQ_IX_APPEND("R %" PRIu32 " %" PRIu32 " %lld", s->seq, off, (long long)rem) == ESP_OK) {
            ESP_LOGW(TAG, "ev-%06u.log left the queue without our ACK (rollback?) — will re-import it",
                     (unsigned)s->seq);
        }
    }
}

/* Boot repair of index states against flash presence and the cursor. */
static void evq_boot_repair_states_locked(void)
{
    for (size_t i = 0; i < s_ix.n; i++) {
        evq_seg_t *s = &s_ix.segs[i];
        s->flash_present = evlog_flash_exists(s->seq, NULL);
    }
    for (size_t i = 0; i < s_ix.n;) {
        evq_seg_t *s = &s_ix.segs[i];
        uint32_t seq = s->seq;
        if (s->state == EVQ_SEG_DELIVERED && seq >= s_rd_seq) {
            /* A DELIVERED line the cursor does not confirm: downgrade. */
            if (s->primary[0] != '\0') {
                (void)EVQ_IX_APPEND("P %" PRIu32 " %08" PRIx32 " %s %s", seq, s->cid, s->primary, s->mirror);
                s = evq_index_find(&s_ix, seq);
                if (s && !s->flash_present) (void)EVQ_IX_APPEND("O %" PRIu32, seq);
            } else {
                (void)EVQ_IX_APPEND("S %" PRIu32 " %lld %lld %" PRIu32 " %" PRIu32 " %08" PRIx32,
                                    seq, (long long)s->first_id, (long long)s->last_id,
                                    s->count, s->bytes, s->crc);
            }
        } else if (s->state == EVQ_SEG_SD_ONLY && s->flash_present) {
            /* Crash inside reclaim: flash copy still there — treat as SPOOLED. */
            (void)EVQ_IX_APPEND("P %" PRIu32 " %08" PRIx32 " %s %s", seq, s->cid, s->primary, s->mirror);
        } else if (s->state == EVQ_SEG_SPOOLED && !s->flash_present) {
            (void)EVQ_IX_APPEND("O %" PRIu32, seq);       /* reclaim removed it before its O line */
        } else if (s->state == EVQ_SEG_FLASH && !s->flash_present && seq < s_rd_seq) {
            (void)EVQ_IX_APPEND("A %" PRIu32, seq);       /* delivered and gone (evicted/archived) */
            continue;                                      /* entry removed; same i */
        } else if ((s->state == EVQ_SEG_SPOOLED || s->state == EVQ_SEG_SD_ONLY) && seq < s_rd_seq) {
            /* Passed by our own blob-confirmed cursor (evq_reconcile) → delivered. */
            (void)EVQ_IX_APPEND("D %" PRIu32, seq);
        }
        s = evq_index_find(&s_ix, seq);
        if (s == NULL) continue;
        i++;
    }
}

/* Discover files, load the index, reconcile the NVS cursor, open the tail for
 * append. Caller holds s_mtx (or is single-threaded at init). */
static esp_err_t evlog_open_locked(void)
{
    if (s_wf != NULL) { fclose(s_wf); s_wf = NULL; }

    mkdir(EVLOG_DIR, 0777);

    /* Segment index. A missing file on a store that already has rotated
     * files is a pre-upgrade store (or a lost index): everything written
     * before now is pre-watermark and the keeper indexes the files. */
    if (!s_ix_ready) {
        bool have_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
        if (evq_index_init(&s_ix, have_psram ? EVQ_INDEX_CAP : EVQ_INDEX_CAP_FALLBACK) != ESP_OK) {
            ESP_LOGE(TAG, "segment index allocation failed — SD overflow disabled this session");
        } else {
            s_ix_ready = true;
        }
    }
    struct stat ist;
    bool index_existed = stat(EVQ_INDEX_PATH, &ist) == 0;
    if (s_ix_ready) {
        if (evq_index_load(&s_ix, EVQ_INDEX_PATH) != ESP_OK) {
            ESP_LOGW(TAG, "segment index read error — continuing with %u valid line(s)", (unsigned)s_ix.lines);
        }
        if (s_ix.bad_lines > 0) {
            ESP_LOGW(TAG, "segment index: %u corrupt/torn line(s) ignored", (unsigned)s_ix.bad_lines);
        }
    }

    uint32_t min_seq = 0, max_seq = 0;
    bool any = evlog_scan_range_locked(&min_seq, &max_seq);
    if (!any) min_seq = max_seq = 1;
    /* The tail is never reclaimed, so it is the newest flash file; but never
     * reuse a seq the index knows (a lost tail after a reclaim of its
     * predecessor would otherwise re-number an SD-only segment). */
    uint32_t ix_min = 0, ix_max = 0;
    for (size_t i = 0; i < s_ix.n; i++) {
        if (s_ix.segs[i].state == EVQ_SEG_REIMPORT) continue;
        if (ix_min == 0 || s_ix.segs[i].seq < ix_min) ix_min = s_ix.segs[i].seq;
        if (s_ix.segs[i].seq > ix_max) ix_max = s_ix.segs[i].seq;
    }
    if (s_ix.n > 0 && s_ix.segs[s_ix.n - 1].seq > ix_max) ix_max = s_ix.segs[s_ix.n - 1].seq;
    s_tail_seq = max_seq;
    if (ix_max >= s_tail_seq) s_tail_seq = ix_max + 1;
    uint32_t q_min = min_seq;
    if (ix_min != 0 && ix_min < q_min) q_min = ix_min;

    /* Cursor: atomic blob vs legacy keys. */
    uint32_t cseq = 0, coff32 = 0, bseq = 0, boff = 0;
    bool foreign = false;
    evq_reconcile_cursor_locked(&cseq, &coff32, &foreign, &bseq, &boff);
    if (cseq == 0) { cseq = q_min; coff32 = 0; }
    if (foreign && s_ix_ready) evq_mark_foreign_range_locked(bseq, boff, cseq);
    /* Clamp to what exists on either medium (a torn offset past EOF is pulled
     * back). Below every copy = a pre-upgrade gap; above = impossible. */
    if (cseq < q_min) { cseq = q_min; coff32 = 0; }
    if (cseq > s_tail_seq) { cseq = s_tail_seq; coff32 = 0; }
    s_rd_seq = cseq;
    s_rd_off = (long)coff32;
    long fsz = 0;
    if (evq_file_size_locked(s_rd_seq, &fsz) && s_rd_off > fsz) s_rd_off = fsz;
    s_cur_validated = false;
    s_cur_consumed = 0;

    if (s_ix_ready) evq_boot_repair_states_locked();

    char path[EVQ_PATH_MAX];
    evlog_file_path(path, sizeof path, s_tail_seq);
    evlog_scan_tail_locked();

    s_wf = fopen(path, "a");
    if (s_wf == NULL) {
        ESP_LOGE(TAG, "open tail %s failed", path);
        return ESP_FAIL;
    }
    s_tail_size = evlog_file_size(s_tail_seq);

    /* Upgrade watermark: first boot of this firmware on this store. */
    if (s_ix_ready && !s_ix.have_watermark) {
        (void)EVQ_IX_APPEND("W %" PRIu32 " %ld", s_tail_seq, s_tail_size);
        if (!index_existed) ESP_LOGI(TAG, "segment index created (watermark ev-%06u.log:%ld)",
                                     (unsigned)s_tail_seq, s_tail_size);
    }

    /* Rotated flash files without an index entry → the keeper indexes them. */
    s_unidx_present = false;
    s_unidx_floor = 0;
    if (any) {
        for (uint32_t seq = min_seq; seq < s_tail_seq; seq++) {
            if (seq < s_rd_seq) continue;
            if (evq_index_find(&s_ix, seq) == NULL && evlog_flash_exists(seq, NULL)) { s_unidx_present = true; break; }
        }
    }

    /* Never hand out an id that collides with a record still stored anywhere.
     * NVS (where next_id's HWM lives) can be wiped by a reflash while the log
     * survives, so reseed above the tail and above every indexed segment. */
    evlog_bump_next_id_locked(s_tail_last_id);
    for (size_t i = 0; i < s_ix.n; i++) evlog_bump_next_id_locked(s_ix.segs[i].last_id);

    s_acks_since_persist = 0;
    evlog_window_clear_locked();
    s_head_block = EVQ_BLOCK_NONE;
    evq_validate_cursor_locked();

    /* First keeper pass as soon as it starts (index pre-upgrade files, repair
     * the card, resume transfers) instead of one period after boot. */
    s_keeper_wake_pending = true;

    ESP_LOGI(TAG, "ready: flash files %u..%u, %u indexed segment(s), cursor seq=%u off=%ld, next_id=%lld",
             (unsigned)min_seq, (unsigned)s_tail_seq, (unsigned)s_ix.n, (unsigned)s_rd_seq, s_rd_off,
             (long long)s_next_id);
    return ESP_OK;
}

/* ── public API ──────────────────────────────────────────────────────── */

/* Boot-only and idempotent: liveness can allocate IDs even if the event-store
 * mount or large line-buffer allocation fails. Start producer tasks only after
 * event_log_init has had its normal opportunity to reconcile the disk frontier. */
esp_err_t event_log_init_ids(void)
{
    if (s_mtx != NULL) return ESP_OK;
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_storage);
    if (s_mtx == NULL) return ESP_ERR_NO_MEM;

    /* next_id resumes at the persisted high-water mark; the first next_id call
     * reserves a fresh block. Gaps across reboots are fine (ids need only be
     * monotonic + unique). */
    uint64_t hwm = 1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u64(h, NVS_KEY_NID, &hwm);
        nvs_close(h);
    }
    if (hwm < 1) hwm = 1;
    s_next_id  = (int64_t)hwm;
    s_id_limit = (int64_t)hwm;

    return ESP_OK;
}

esp_err_t event_log_init(void)
{
    esp_err_t id_err = event_log_init_ids();
    if (id_err != ESP_OK) return id_err;
    esp_err_t line_err = evlog_allocate_line_buffer();
    if (line_err != ESP_OK) return line_err;

    /* The internal partition is mounted by app_main before this runs and cannot
     * disappear afterwards — open unconditionally. SD-side repair is the keeper's
     * job: measurement and publishing never wait on the card. */
    s_available = (evlog_open_locked() == ESP_OK);
    if (!s_available) ESP_LOGW(TAG, "event log unavailable");
    return ESP_OK;
}

void event_log_set_reset_notifier(void (*fn)(void))
{
    s_reset_notifier = fn;
}

void event_log_set_sd_parked(bool parked)
{
    s_sd_parked = parked;
    if (!parked) evq_keeper_notify();
}

esp_err_t event_log_prepare_shutdown(void)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;

    /* Pre-reboot drain: fsync + close the tail and persist the read cursor.
     * Records are already fsync'd per store; this persists the batched cursor
     * so fewer duplicates replay. Bounded lock wait — a reboot must not hang
     * on a stuck writer. */
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "pre-reboot flush skipped (lock busy) — last persisted cursor stands");
        return ESP_ERR_TIMEOUT;
    }
    if (s_wf != NULL) {
        evlog_flush_writer_locked();
        fclose(s_wf);
        s_wf = NULL;
    }
    evlog_persist_cursor_locked();
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t event_log_next_id(int64_t *out_id)
{
    if (out_id == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_next_id >= s_id_limit) {
        s_id_limit = s_next_id + EVLOG_ID_BLOCK;
        evlog_persist_nid_locked();
    }
    *out_id = s_next_id++;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* Roll the tail to a fresh file: fsync + close, register the closed file in the
 * index with its running statistics, open the next seq. On a full store the
 * rotation is deferred (keep appending to the old tail). Returns false only if
 * no tail could be (re)opened at all. Caller holds s_mtx. */
static bool evlog_rotate_locked(void)
{
    if (evlog_flush_writer_locked() != ESP_OK) evstore_report_io_error();
    fclose(s_wf);
    s_wf = NULL;
    uint32_t old = s_tail_seq;
    uint32_t count = s_tail_count, crc = s_tail_crc, bytes = (uint32_t)s_tail_size;
    int64_t first = s_tail_first_id, last = s_tail_last_id;
    EVQ_FAULT_POINT("store.rotate_after_close_before_open");
    s_tail_seq++;
    char path[EVQ_PATH_MAX];
    evlog_file_path(path, sizeof path, s_tail_seq);
    s_wf = fopen(path, "a");
    if (s_wf == NULL && evlog_full_recovery_locked()) {
        s_wf = fopen(path, "a");   /* eviction freed space — retry once */
    }
    if (s_wf == NULL) {
        /* Still no room. Roll the sequence back and keep appending to the
         * previous tail: writer and reader must never point at a file that does
         * not exist (a phantom tail reads as "queue drained"). */
        s_tail_seq = old;
        evlog_file_path(path, sizeof path, s_tail_seq);
        s_wf = fopen(path, "a");
        if (s_wf == NULL) return false;
        ESP_LOGW(TAG, "rotate deferred (store full) — still appending to %s", path);
        return true;
    }
    fsync(fileno(s_wf));
    evq_register_rotated_locked(old, first, last, count, bytes, crc);
    EVQ_FAULT_POINT("store.rotate_after_index_seg");
    s_tail_size = 0;
    s_tail_count = 0;
    s_tail_crc = 0;
    s_tail_first_id = s_tail_last_id = 0;
    if (s_ix.n >= s_ix.cap) ESP_LOGW(TAG, "segment index full (%u) — new files stay flash-only", (unsigned)s_ix.cap);
    return true;
}

/* Record one appended line in the tail's running statistics. */
static void evlog_tail_account(const char *const *parts, const size_t *lens, size_t n, int64_t id)
{
    for (size_t i = 0; i < n; i++) s_tail_crc = evq_crc32(s_tail_crc, parts[i], lens[i]);
    if (s_tail_count == 0) s_tail_first_id = id;
    s_tail_last_id = id;
    s_tail_count++;
}

static bool evq_pressure_locked(uint64_t *freeb_out)
{
    uint64_t freeb = 0, total = 0;
    if (evstore_space(&freeb, &total) != ESP_OK || total == 0) return false;
    if (freeb_out) *freeb_out = freeb;
    return freeb * 100U < total * EVQ_PRESSURE_PCT;
}

static esp_err_t event_log_store_impl(const measurement_event_desc_t *desc)
{
    if (desc == NULL || desc->payload_json == NULL ||
        desc->tag == NULL || desc->tag[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) {
        s_refused_unavailable++;      /* racy increment is fine: a counter */
        return ESP_ERR_TIMEOUT;
    }
    if (!s_available) {
        s_refused_unavailable++;
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Self-heal: a prior rotate/open failure may have closed the tail without
     * latching the log offline. On a FULL partition the reopen itself ENOSPCs,
     * so evict delivered copies and retry once before declaring it unwritable. */
    if (s_wf == NULL) {
        esp_err_t ro = evlog_reopen_tail_locked();
        if (ro != ESP_OK && evlog_full_recovery_locked()) {
            ro = evlog_reopen_tail_locked();
        }
        if (ro != ESP_OK) {
            if (s_write_full) {
                s_dropped++; s_refused_full++;
                xSemaphoreGive(s_mtx);
                return ESP_ERR_NO_MEM;   /* full is not a media fault */
            }
            s_dropped++; s_refused_media++;
            xSemaphoreGive(s_mtx);
            evstore_report_io_error();
            return ESP_FAIL;
        }
    }
    /* Storage-full admission control: a full store is healthy, so refuse cleanly
     * and let the drain + keeper free space. */
    if (s_write_full) {
        uint64_t freeb = 0;
        if (evstore_free_bytes(&freeb) == ESP_OK && freeb < EVLOG_MIN_FREE_BYTES) {
            (void)evlog_evict_synced_locked();
        }
        if (evstore_free_bytes(&freeb) == ESP_OK && freeb >= EVLOG_MIN_FREE_BYTES) {
            s_write_full = false;
            ESP_LOGI(TAG, "store space recovered (%llu B free) — writes resumed",
                     (unsigned long long)freeb);
        } else {
            s_dropped++; s_refused_full++;
            evq_keeper_notify();
            xSemaphoreGive(s_mtx);
            return ESP_ERR_NO_MEM;
        }
    }

    const int64_t measure_id = desc->measure_id;
    char chan[12], dev[24], tag[16];
    sanitize_field(chan, sizeof chan, desc->channel);
    sanitize_field(dev,  sizeof dev,  desc->device);
    sanitize_field(tag,  sizeof tag,  desc->tag);
    /* cmd_raw is variable-length (a full multi-segment "arrun …" can be ~520 B),
     * so it gets its own heap-sanitized buffer and its own write. */
    const char *cmd_src = (desc->cmd_raw != NULL) ? desc->cmd_raw : "";
    size_t cmd_cap = strlen(cmd_src) + 1;
    char *cmd = malloc(cmd_cap);
    if (cmd == NULL) {
        s_refused_unavailable++;
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NO_MEM;
    }
    sanitize_field(cmd, cmd_cap, cmd_src);
    const char *meta = (desc->metadata_json != NULL && desc->metadata_json[0] != '\0')
                       ? desc->metadata_json : "";
    const char *payload_json = desc->payload_json;

    char hdr1[96], hdr2[48];
    int h1 = snprintf(hdr1, sizeof hdr1, "%lld\t%s\t%s\t%s\t",
                      (long long)measure_id, chan, dev, tag);
    int h2 = snprintf(hdr2, sizeof hdr2, "\t%lld\t%lld\t",
                      (long long)desc->start_ms, (long long)desc->end_ms);
    if (h1 < 0 || h1 >= (int)sizeof hdr1 || h2 < 0 || h2 >= (int)sizeof hdr2) {
        free(cmd);
        s_refused_media++;
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    size_t clen  = strlen(cmd);
    size_t mlen  = strlen(meta);
    size_t plen  = strlen(payload_json);
    size_t total = (size_t)h1 + clen + (size_t)h2 + mlen + 1 /*tab*/ + plen + 1 /*\n*/;
    if (total >= s_max_record) {
        ESP_LOGE(TAG, "record too large (%u B) for active %u-B cap (max record %u B), id %lld — dropped",
                 (unsigned)total, (unsigned)s_line_cap, (unsigned)s_max_record,
                 (long long)measure_id);
        s_dropped++; s_refused_too_large++;
        free(cmd);
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Roll to a fresh file before the tail would exceed the rotate threshold. */
    if (s_tail_size > 0 && s_tail_size + (long)total > EVLOG_ROTATE_BYTES) {
        if (!evlog_rotate_locked()) {
            ESP_LOGE(TAG, "rotate: reopen of previous tail failed — dropping record");
            s_dropped++; s_refused_media++;
            free(cmd);
            xSemaphoreGive(s_mtx);
            evstore_report_io_error();
            return ESP_FAIL;
        }
    }

    EVQ_FAULT_POINT("store.before_write");
    const char *parts[7] = { hdr1, cmd, hdr2, meta, "\t", payload_json, "\n" };
    const size_t lens[7] = { (size_t)h1, clen, (size_t)h2, mlen, 1, plen, 1 };
    size_t w = 0;
    for (size_t i = 0; i < 7; i++) w += fwrite(parts[i], 1, lens[i], s_wf);
    EVQ_FAULT_POINT("store.after_write_before_fsync");
    bool durable = (w == total) && evlog_flush_writer_locked() == ESP_OK;
    if (!durable) {
        /* A failed/short write or sync leaves a torn or non-durable record. Roll
         * the file back to the last acknowledged boundary so it can neither
         * merge with the next record nor surface later as a "refused" record
         * that is delivered anyway. */
        int saved_errno = errno;
        ESP_LOGE(TAG, "store_event: write/sync failed (%u/%u) for id %lld — rolling back",
                 (unsigned)w, (unsigned)total, (long long)measure_id);
        clearerr(s_wf);
        fflush(s_wf);
        if (ftruncate(fileno(s_wf), s_tail_size) != 0 || fsync(fileno(s_wf)) != 0) {
            /* Cannot prove the rollback: close the writer so the next store
             * reopens at the true file end (and re-derive stats at boot). */
            ESP_LOGW(TAG, "store_event: rollback truncate failed — writer reset");
            fclose(s_wf);
            s_wf = NULL;
        }
        free(cmd);
        s_dropped++;
        uint64_t freeb = 0;
        bool full = (saved_errno == ENOSPC) ||
                    (evstore_free_bytes(&freeb) == ESP_OK && freeb < EVLOG_MIN_FREE_BYTES);
        if (full) {
            s_refused_full++;
            (void)evlog_full_recovery_locked();
            xSemaphoreGive(s_mtx);
            return ESP_ERR_NO_MEM;      /* full is not a media fault: no report_io_error */
        }
        s_refused_media++;
        xSemaphoreGive(s_mtx);
        evstore_report_io_error();
        return ESP_FAIL;
    }
    evstore_report_io_ok();
    evlog_tail_account(parts, lens, 7, measure_id);
    free(cmd);
    s_tail_size += (long)total;
    s_stores_since_archive++;
    EVQ_FAULT_POINT("store.after_fsync_before_return");

    uint64_t freeb = 0;
    bool pressure = evq_pressure_locked(&freeb);
    if (pressure) s_pressure_notifies++;
    bool wake = pressure || s_stores_since_archive == EVLOG_ARCHIVE_EVERY_N;
    xSemaphoreGive(s_mtx);
    if (wake) evq_keeper_notify();
    return ESP_OK;
}

/* Frontier helper: set the head block and return NOT_FINISHED. */
static esp_err_t evq_block_locked(uint8_t why)
{
    if (s_head_block != why) {
        ESP_LOGW(TAG, "delivery waits at ev-%06u.log:%ld — %s (cursor held; nothing skipped)",
                 (unsigned)s_rd_seq, s_rd_off, event_log_block_name(why));
    }
    s_head_block = why;
    return ESP_ERR_NOT_FINISHED;
}

static esp_err_t event_log_claim_impl(measurement_event_t *out)
{
    memset(out, 0, sizeof *out);
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Claim-order contract:
     *   1. the oldest reverted, unacked slot already in the window;
     *   2. otherwise the record immediately after the claim tail;
     *   3. never a new offset while any reverted slot exists.
     * ACKed-but-not-prefix slots remain occupied but are never re-published.
     * This ordering is what repairs a hole before extending the window. */
    for (size_t i = 0; i < s_window_count; i++) {
        evlog_window_slot_t *slot = &s_window[i];
        if (slot->acked || slot->claimed) continue;

        evq_rd_t rd;
        uint8_t block = EVQ_BLOCK_NONE;
        esp_err_t oe = evq_open_read_locked(slot->seq, &rd, &block);
        if (oe != ESP_OK) {
            esp_err_t r = (oe == ESP_ERR_NOT_FINISHED) ? evq_block_locked(block) : ESP_FAIL;
            ESP_LOGE(TAG, "re-claim of id=%lld at %u:%ld unavailable — holding window",
                     (long long)slot->measure_id, (unsigned)slot->seq, slot->off);
            xSemaphoreGive(s_mtx);
            return r;
        }
        if (fseek(rd.f, slot->off, SEEK_SET) != 0 || fgets(s_line, s_line_cap, rd.f) == NULL) {
            evq_rd_close(&rd);
            ESP_LOGE(TAG, "re-claim failed for id=%lld at %u:%ld — holding window",
                     (long long)slot->measure_id, (unsigned)slot->seq, slot->off);
            xSemaphoreGive(s_mtx);
            return ESP_FAIL;
        }
        evq_rd_close(&rd);

        size_t len = strlen(s_line);
        int64_t disk_id = (int64_t)strtoll(s_line, NULL, 10);
        if ((long)len != slot->len || len == 0 || s_line[len - 1] != '\n' ||
            disk_id != slot->measure_id) {
            ESP_LOGE(TAG, "re-claim identity mismatch at %u:%ld: id=%lld len=%u, expected id=%lld len=%ld",
                     (unsigned)slot->seq, slot->off, (long long)disk_id,
                     (unsigned)len, (long long)slot->measure_id, slot->len);
            xSemaphoreGive(s_mtx);
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t parsed = parse_record(s_line, len, out);
        if (parsed != ESP_OK || out->measure_id != slot->measure_id) {
            measurement_event_free(out);
            xSemaphoreGive(s_mtx);
            return parsed == ESP_OK ? ESP_ERR_INVALID_STATE : parsed;
        }
        slot->claimed = true;
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }

    if (s_window_count >= PUBLISH_WINDOW_SLOTS) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    /* The scan position follows the claim tail, never the cursor unless the
     * window is empty. */
    evlog_normalize_cursor_locked();
    if (s_window_count == 0) evq_validate_cursor_locked();
    uint32_t scan_seq = s_rd_seq;
    long scan_off = s_rd_off;
    if (s_window_count > 0) {
        const evlog_window_slot_t *last = &s_window[s_window_count - 1U];
        scan_seq = last->seq;
        scan_off = last->off + last->len;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (int guard = 0; guard < 100000; guard++) {
        bool at_cursor = s_window_count == 0 && scan_seq == s_rd_seq && scan_off == s_rd_off;
        bool is_tail = (scan_seq >= s_tail_seq);
        if (is_tail && evlog_flush_writer_locked() != ESP_OK) {
            evstore_report_io_error();
            result = ESP_FAIL;
            break;
        }

        evq_rd_t rd;
        uint8_t block = EVQ_BLOCK_NONE;
        esp_err_t oe = evq_open_read_locked(scan_seq, &rd, &block);
        if (oe == ESP_ERR_NOT_FINISHED) {
            /* An indexed obligation that cannot be read right now (card absent,
             * parked, swapped, file missing or both copies corrupt): WAIT. The
             * cursor never passes it; with claims already outstanding, report
             * window-bound so the drain keeps consuming their ACKs. */
            result = at_cursor ? evq_block_locked(block) : ESP_ERR_INVALID_STATE;
            break;
        }
        if (oe == ESP_ERR_NOT_FOUND) {
            if (is_tail && s_wf == NULL) {
                /* Writer-down state: a rotate onto a full store advanced
                 * s_tail_seq past a file that was never created. A phantom tail
                 * must not read as "queue drained": evict, roll the tail back
                 * to the newest real file, reopen it, and retry once. */
                (void)evlog_full_recovery_locked();
                uint32_t min_seq = 0, max_seq = 0;
                if (evlog_scan_range_locked(&min_seq, &max_seq) &&
                    max_seq < s_tail_seq && evq_index_find(&s_ix, max_seq) == NULL) {
                    ESP_LOGW(TAG, "tail ev-%06u.log missing with writer down — rolling back to ev-%06u.log",
                             (unsigned)s_tail_seq, (unsigned)max_seq);
                    s_tail_seq = max_seq;
                    evlog_scan_tail_locked();
                }
                if (evlog_reopen_tail_locked() == ESP_OK) continue;
                result = ESP_ERR_NOT_FOUND;
                break;
            }
            if (is_tail) { result = ESP_ERR_NOT_FOUND; break; }
            if (at_cursor) {
                /* Not on flash, not in the index: a pre-upgrade gap (files the
                 * old firmware deleted). Nothing can recover it; count it. */
                ESP_LOGW(TAG, "ev-%06u.log missing at cursor and never indexed — passing gap", (unsigned)scan_seq);
                s_skipped++;
                s_skipped_unindexed_gap++;
                s_rd_seq++; s_rd_off = 0; s_cur_consumed = 0;
                evlog_persist_cursor_locked();
                scan_seq = s_rd_seq;
                scan_off = s_rd_off;
                continue;
            }
            /* Do not create a later-file slot across a gap. */
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (oe != ESP_OK) {
            if (!is_tail) evstore_report_io_error();
            result = is_tail ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            break;
        }
        if (at_cursor) s_head_block = EVQ_BLOCK_NONE;
        if (fseek(rd.f, scan_off, SEEK_SET) != 0) {
            evq_rd_close(&rd);
            result = ESP_ERR_NOT_FOUND;
            break;
        }

        char *got = fgets(s_line, s_line_cap, rd.f);
        if (got == NULL) {
            if (ferror(rd.f)) {
                clearerr(rd.f); evq_rd_close(&rd);
                if (rd.medium == EVQ_MEDIUM_SD) sdcard_report_io_error(); else evstore_report_io_error();
                result = ESP_FAIL;
                break;
            }
            evq_rd_close(&rd);
            if (!is_tail) {
                if (at_cursor) {
                    evq_cursor_leave_file_locked();   /* EOF reached by the ACK prefix */
                    scan_seq = s_rd_seq; scan_off = s_rd_off;
                    continue;
                }
                scan_seq++;
                scan_off = 0;
                continue;
            }
            result = ESP_ERR_NOT_FOUND;
            break;
        }

        size_t len = strlen(got);
        bool complete = (len > 0 && got[len - 1] == '\n');
        if (!complete) {
            if (len == s_line_cap - 1U) {            /* over-long/corrupt: no '\n' within active cap */
                long skipped = (long)len;
                char *more;
                bool framed = false;
                while ((more = fgets(s_line, s_line_cap, rd.f)) != NULL) {
                    size_t l2 = strlen(more);
                    skipped += (long)l2;
                    if (l2 > 0 && more[l2 - 1] == '\n') { framed = true; break; }
                }
                if (ferror(rd.f)) {                  /* over-long scan hit a read error → don't skip */
                    clearerr(rd.f); evq_rd_close(&rd);
                    evstore_report_io_error();
                    result = ESP_FAIL;
                    break;
                }
                evq_rd_close(&rd);
                if (!at_cursor) { result = ESP_ERR_INVALID_STATE; break; }
                if (!framed && is_tail) { result = ESP_ERR_NOT_FOUND; break; }   /* still being written? */
                /* Preserve the intact bytes BEFORE the cursor moves. */
                if (!evq_quarantine_range_locked(scan_seq, scan_off, skipped)) {
                    result = ESP_FAIL;
                    break;
                }
                ESP_LOGW(TAG, "quarantined over-long record seq=%u off=%ld (%ld B)",
                         (unsigned)scan_seq, scan_off, skipped);
                evq_account_quarantined_locked(EVQ_Q_MALFORMED, scan_seq, scan_off);
                s_rd_off += skipped;
                s_cur_consumed += framed ? 1 : 0;
                scan_off = s_rd_off;
                evlog_persist_cursor_locked();
                continue;
            }
            if (ferror(rd.f)) {                      /* partial line due to a read error, not a torn record */
                clearerr(rd.f); evq_rd_close(&rd);
                evstore_report_io_error();
                result = ESP_FAIL;
                break;
            }
            evq_rd_close(&rd);
            if (!is_tail && at_cursor) {
                /* Torn tail of a closed, UNINDEXED (pre-upgrade) file — indexed
                 * files are CRC-verified before reading, so they never get here.
                 * Keep the fragment, then leave the file. */
                if (!evq_quarantine_range_locked(scan_seq, scan_off, (long)len)) {
                    result = ESP_FAIL;
                    break;
                }
                ESP_LOGW(TAG, "partial record at end of rotated ev-%06u.log — quarantined", (unsigned)scan_seq);
                evq_account_quarantined_locked(EVQ_Q_MALFORMED, scan_seq, scan_off);
                s_rd_off += (long)len;
                evlog_persist_cursor_locked();
                evq_cursor_leave_file_locked();
                scan_seq = s_rd_seq; scan_off = 0;
                continue;
            }
            result = is_tail ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
            break;
        }
        evq_rd_close(&rd);

        /* Raw-record bytes are a conservative, persistence-owned first gate.
         * device_commands applies the exact envelope-byte gate before publish;
         * both serialize a lone over-budget item, then enforce the byte ceiling
         * once anything is outstanding. */
        if (s_window_bytes > 0 &&
            (s_window_bytes >= PUBLISH_WINDOW_BYTES ||
             len > PUBLISH_WINDOW_BYTES - s_window_bytes)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        int64_t record_id = (int64_t)strtoll(got, NULL, 10);
        esp_err_t pr = parse_record(got, len, out);
        if (pr == ESP_OK) {
            evlog_window_slot_t *slot = &s_window[s_window_count++];
            *slot = (evlog_window_slot_t) {
                .measure_id = out->measure_id,
                .seq        = scan_seq,
                .off        = scan_off,
                .len        = (long)len,
                .claimed    = true,
                .acked      = false,
            };
            s_window_bytes += len;
            s_oom_head_id     = 0;   /* made progress — clear OOM strike tracking */
            s_oom_strikes     = 0;
            result = ESP_OK;
            break;
        }
        if (pr == ESP_ERR_NO_MEM) {
            /* Fragmented heap can't strdup this (oversized) record. Retrying it
             * forever head-of-line-blocks the whole backlog into an hourly reboot
             * loop (audit D2). After EVLOG_OOM_STUCK_MAX strikes on the SAME head,
             * quarantine it (intact copy first) so the drain advances. */
            if (!at_cursor) {
                result = ESP_ERR_NO_MEM;
                break;
            }
            if (record_id == s_oom_head_id) s_oom_strikes++;
            else { s_oom_head_id = record_id; s_oom_strikes = 1; }
            if (s_oom_strikes >= EVLOG_OOM_STUCK_MAX &&
                evq_quarantine_range_locked(scan_seq, scan_off, (long)len)) {
                ESP_LOGW(TAG, "OOM-stuck record id=%lld quarantined after %u strikes — drain unblocked",
                         (long long)record_id, (unsigned)s_oom_strikes);
                evq_account_quarantined_locked(EVQ_Q_POISON, scan_seq, scan_off);
                s_rd_off += (long)len;
                s_cur_consumed++;
                scan_off = s_rd_off;
                s_oom_head_id = 0; s_oom_strikes = 0;
                evlog_persist_cursor_locked();
                continue;
            }
            result = ESP_ERR_NO_MEM;                 /* don't consume — retry next drain */
            break;
        }
        /* Malformed records may be passed only at the cursor frontier, and only
         * after their intact bytes are durable in quarantine. With claim!=cursor,
         * stop and let earlier slots close first (the ticket-04 m5 rule). */
        if (!at_cursor) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (!evq_quarantine_range_locked(scan_seq, scan_off, (long)len)) {
            result = ESP_FAIL;                       /* Q3: cursor stays */
            break;
        }
        ESP_LOGW(TAG, "quarantined malformed record seq=%u off=%ld len=%u",
                 (unsigned)s_rd_seq, s_rd_off, (unsigned)len);
        evq_account_quarantined_locked(EVQ_Q_MALFORMED, scan_seq, scan_off);
        s_rd_off += (long)len;
        s_cur_consumed++;
        scan_off = s_rd_off;
        evlog_persist_cursor_locked();
    }

    xSemaphoreGive(s_mtx);
    return result;
}

esp_err_t event_log_mark_event_synced(int64_t measure_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int idx = evlog_window_find_locked(measure_id);
    if (idx < 0 || !s_window[idx].claimed || s_window[idx].acked) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    /* Clearing `claimed` records that the MQTT latch has already been detached
     * into the completion queue.  Later ACKed slots remain in-place; only the
     * helper's slot-zero loop is allowed to advance the cursor. */
    s_window[idx].claimed = false;
    s_window[idx].acked = true;
    evlog_advance_acked_prefix_locked();
    bool reset_pending = s_reset_pending;
    s_reset_pending = false;
    xSemaphoreGive(s_mtx);
    if (reset_pending && s_reset_notifier != NULL) s_reset_notifier();
    return ESP_OK;
}

esp_err_t event_log_mark_event_pending(int64_t measure_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int idx = evlog_window_find_locked(measure_id);
    if (idx < 0 || !s_window[idx].claimed || s_window[idx].acked) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    /* Revert exactly one slot.  Its seq/off/len charge remains in the window,
     * and claim_next's FIFO scan returns it before admitting any newer offset. */
    s_window[idx].claimed = false;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

static esp_err_t event_log_quarantine_impl(int64_t measure_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available) { xSemaphoreGive(s_mtx); return ESP_ERR_NOT_SUPPORTED; }

    /* Window-safety contract: quarantine is deliberately a frontier-only
     * operation. Re-read s_rd_seq/s_rd_off under s_mtx and verify measure_id
     * before any archive or cursor mutation. */
    if (s_window_count > 0 &&
        (s_window[0].measure_id != measure_id || s_window[0].seq != s_rd_seq ||
         s_window[0].off != s_rd_off || s_window[0].claimed || s_window[0].acked)) {
        ESP_LOGW(TAG, "quarantine refused for non-pending frontier id=%lld; frontier id=%lld at %u:%ld",
                 (long long)measure_id, (long long)s_window[0].measure_id,
                 (unsigned)s_window[0].seq, s_window[0].off);
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    evq_rd_t rd;
    uint8_t block;
    if (evq_open_read_locked(s_rd_seq, &rd, &block) != ESP_OK) { xSemaphoreGive(s_mtx); return ESP_ERR_NOT_FOUND; }
    if (fseek(rd.f, s_rd_off, SEEK_SET) != 0 || fgets(s_line, s_line_cap, rd.f) == NULL) {
        evq_rd_close(&rd);
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    evq_rd_close(&rd);

    size_t len = strlen(s_line);
    if (len == 0 || s_line[len - 1] != '\n') {
        /* Torn/over-long record: claim's own quarantine logic owns those cases. */
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    if ((int64_t)strtoll(s_line, NULL, 10) != measure_id) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    /* Archive FIRST — the record is only passed once it is durably elsewhere.
     * If the copy fails, stay put and let the caller retry next cycle. */
    if (!evq_quarantine_range_locked(s_rd_seq, s_rd_off, (long)len)) {
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    evq_account_quarantined_locked(EVQ_Q_POISON, s_rd_seq, s_rd_off);

    /* Advance past exactly the cursor record without pretending it had a PUBACK. */
    if (s_window_count > 0) {
        s_rd_seq = s_window[0].seq;
        s_rd_off = s_window[0].off + s_window[0].len;
        evlog_window_pop_front_locked();
    } else {
        s_rd_off += (long)len;
    }
    s_cur_consumed++;
    evlog_advance_acked_prefix_locked();
    evlog_normalize_cursor_locked();
    evlog_persist_cursor_locked();
    s_acks_since_persist = 0;
    bool reset_pending = s_reset_pending;
    s_reset_pending = false;
    xSemaphoreGive(s_mtx);

    if (reset_pending && s_reset_notifier != NULL) s_reset_notifier();

    ESP_LOGW(TAG, "quarantined event id=%lld (%u B) -> %s — cursor advanced, drain unblocked",
             (long long)measure_id, (unsigned)len, EVLOG_QUARANTINE);
    return ESP_OK;
}

static esp_err_t event_log_rewind_impl(uint32_t seq, uint32_t *out_seq, int64_t *out_pending)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available) { xSemaphoreGive(s_mtx); return ESP_ERR_NOT_SUPPORTED; }

    /* Clamp to files the QUEUE can still read: flash files plus SPOOLED/SD_ONLY
     * segments (never archived ones). seq==0 means "the oldest such file". */
    uint32_t min_seq = 0, max_seq = 0;
    bool any = evlog_scan_range_locked(&min_seq, &max_seq);
    for (size_t i = 0; i < s_ix.n; i++) {
        const evq_seg_t *s = &s_ix.segs[i];
        if (s->state != EVQ_SEG_SPOOLED && s->state != EVQ_SEG_SD_ONLY) continue;
        if (!any || s->seq < min_seq) { min_seq = s->seq; if (!any) max_seq = s->seq; any = true; }
    }
    if (!any) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t target = (seq == 0) ? min_seq : seq;
    if (target < min_seq) target = min_seq;
    if (target > max_seq) target = max_seq;
    /* Rewind only: never advance the cursor forward. */
    if (target > s_rd_seq) target = s_rd_seq;

    s_rd_seq = target;
    s_rd_off = 0;
    s_cur_consumed = 0;
    s_cur_validated = true;
    /* Rewind abandons every RAM-only claim. device_commands_abort_inflight()
     * clears the peer msg-id table and completion queue before the CLI invokes
     * this path, so late PUBACKs cannot address the rebuilt window. */
    evlog_window_clear_locked();
    evlog_persist_cursor_locked();
    /* DELIVERED segments the rewound cursor now precedes rejoin the queue. */
    evq_boot_repair_states_locked();

    evq_counts_t c;
    evq_counts_locked(&c);
    if (out_seq)     *out_seq     = s_rd_seq;
    if (out_pending) *out_pending = c.pending;
    xSemaphoreGive(s_mtx);

    ESP_LOGW(TAG, "cursor rewound to seq=%u off=0 — %lld%s record(s) pending, will re-publish",
             (unsigned)target, (long long)c.pending, c.exact ? "" : "+");
    return ESP_OK;
}

esp_err_t event_log_cursor_info(uint32_t *rd_seq, uint32_t *rd_off, uint32_t *tail_seq)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (rd_seq)   *rd_seq   = s_rd_seq;
    if (rd_off)   *rd_off   = (uint32_t)s_rd_off;
    if (tail_seq) *tail_seq = s_tail_seq;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t event_log_db_stats(bool *available, int64_t *total,
                             int64_t *pending, int64_t *next_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    evq_counts_t c;
    evq_counts_locked(&c);
    if (available) *available = s_available;
    if (total)     *total     = c.pending;
    if (pending)   *pending   = c.pending;
    if (next_id)   *next_id   = s_next_id;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* Is the delivery head waiting on an unreadable SD-only segment? True once a
 * claim hit it, and also derivable without one (right after boot, before the
 * drain has tried): the cursor's segment has no flash copy and its card is not
 * usable. No FS operation — mismatched cards get zero operations. */
static uint8_t evq_head_waiting_locked(void)
{
    if (s_head_block != EVQ_BLOCK_NONE) return s_head_block;
    if (s_rd_seq >= s_tail_seq) return EVQ_BLOCK_NONE;
    const evq_seg_t *s = evq_index_find(&s_ix, s_rd_seq);
    if (s == NULL || s->flash_present || s->primary[0] == '\0') return EVQ_BLOCK_NONE;
    switch (s_sd_state) {
    case EVQ_SD_ABSENT:   return EVQ_BLOCK_SD_ABSENT;
    case EVQ_SD_LOST:     return EVQ_BLOCK_SD_LOST;
    case EVQ_SD_PARKED:   return EVQ_BLOCK_SD_PARKED;
    case EVQ_SD_MISMATCH: return EVQ_BLOCK_SD_MISMATCH;
    default:              return s->cid != s_sd_last_cid ? EVQ_BLOCK_SD_MISMATCH : EVQ_BLOCK_NONE;
    }
}

static uint8_t evq_blocked_reason_locked(void)
{
    if (!s_write_full) return EVQ_BLOCKED_NONE;
    if (evq_head_waiting_locked() != EVQ_BLOCK_NONE) return EVQ_BLOCKED_BACKLOG_WAITING;
    if (s_ix_ready && s_ix.n >= s_ix.cap) return EVQ_BLOCKED_INDEX_CAP;
    switch (s_sd_state) {
    case EVQ_SD_ABSENT: case EVQ_SD_LOST: case EVQ_SD_PARKED: return EVQ_BLOCKED_SD_UNAVAILABLE;
    case EVQ_SD_MISMATCH: return EVQ_BLOCKED_SD_MISMATCH;
    case EVQ_SD_FULL:     return EVQ_BLOCKED_SD_FULL;
    default: break;
    }
    if (s_sd_backoff_active) return EVQ_BLOCKED_SD_ERROR;
    return EVQ_BLOCKED_TRANSFER_PENDING;
}

esp_err_t event_log_health(evlog_health_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    memset(out, 0, sizeof *out);
    evq_sd_observe_locked();
    evq_counts_t c;
    evq_counts_locked(&c);
    out->available     = s_available;
    out->write_full    = s_write_full;
    out->pending       = c.pending;
    out->pending_exact = c.exact;
    out->flash_pending = c.flash;
    out->sd_pending    = c.sd;
    out->reimport_pending = c.reimport;
    /* Strict original order: while the head waits on an unreadable copy the
     * drain can reach NOTHING, so nothing is deliverable (and the no-PUBACK
     * watchdog must not reboot-loop over a missing card). */
    uint8_t head = evq_head_waiting_locked();
    out->deliverable_pending = (head != EVQ_BLOCK_NONE) ? 0 : c.pending - c.reimport;
    out->next_id       = s_next_id;
    out->last_acked_id = s_last_acked_id;
    out->skipped       = s_skipped;
    out->dropped       = s_dropped;
    out->rd_seq        = s_rd_seq;
    out->tail_seq      = s_tail_seq;
    out->sd_state      = s_head_block == EVQ_BLOCK_BACKLOG_MISSING ? EVQ_SD_BACKLOG_MISSING
                       : s_head_block == EVQ_BLOCK_BACKLOG_CORRUPT ? EVQ_SD_BACKLOG_CORRUPT
                       : s_sd_state;
    out->head_block    = head;
    out->storage_blocked = s_write_full;
    out->blocked_reason  = evq_blocked_reason_locked();
    out->refused_full        = s_refused_full;
    out->refused_media       = s_refused_media;
    out->refused_too_large   = s_refused_too_large;
    out->refused_unavailable = s_refused_unavailable;
    out->quarantined_poison    = s_quarantined_poison;
    out->quarantined_malformed = s_quarantined_malformed;
    out->skipped_unindexed_gap = s_skipped_unindexed_gap;
    out->corrupt_detected      = s_corrupt_detected;
    out->corrupt_medium        = s_corrupt_medium;
    out->spool_files      = s_spool_files;
    out->spool_errors     = s_spool_errors;
    out->mirror_used      = s_mirror_used;
    out->reclaimed_files  = s_reclaimed_files;
    out->archived_files   = s_archived_files;
    out->reimported_files = s_reimported_files;
    out->pressure_notifies = s_pressure_notifies;
    out->sd_bursts        = s_sd_bursts;
    out->sd_retired_names = s_sd_retired_names;
    out->sd_bad_copies    = s_sd_bad_copies;
    out->index_segments   = (uint32_t)s_ix.n;
    out->index_cap        = (uint32_t)s_ix.cap;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* ── public entry points: SD RW-gate wrappers ───────────────────────────── */
esp_err_t event_log_store_event(const measurement_event_desc_t *desc)
{
    if (desc == NULL || desc->payload_json == NULL ||
        desc->tag == NULL || desc->tag[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!evstore_io_begin()) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t rc = event_log_store_impl(desc);
    evstore_io_end();
    return rc;
}

esp_err_t event_log_claim_next_event(measurement_event_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!evstore_io_begin()) { memset(out, 0, sizeof *out); return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t rc = event_log_claim_impl(out);
    evstore_io_end();
    return rc;
}

esp_err_t event_log_quarantine_event(int64_t measure_id)
{
    if (!evstore_io_begin()) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t rc = event_log_quarantine_impl(measure_id);
    evstore_io_end();
    return rc;
}

esp_err_t event_log_rewind(uint32_t seq, uint32_t *out_seq, int64_t *out_pending)
{
    if (!evstore_io_begin()) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t rc = event_log_rewind_impl(seq, out_seq, out_pending);
    evstore_io_end();
    return rc;
}

/* ── fn getters ──────────────────────────────────────────────────────── */

measurement_next_id_fn            event_log_get_next_id_fn(void)            { return event_log_next_id; }
measurement_store_event_fn        event_log_get_store_event_fn(void)        { return event_log_store_event; }
measurement_claim_next_event_fn   event_log_get_claim_next_event_fn(void)   { return event_log_claim_next_event; }
measurement_mark_event_synced_fn  event_log_get_mark_event_synced_fn(void)  { return event_log_mark_event_synced; }
measurement_mark_event_pending_fn event_log_get_mark_event_pending_fn(void) { return event_log_mark_event_pending; }
measurement_quarantine_fn         event_log_get_quarantine_fn(void)         { return event_log_quarantine_event; }
measurement_db_stats_fn           event_log_get_db_stats_fn(void)           { return event_log_db_stats; }

esp_err_t event_log_free_bytes(uint64_t *out_free)
{
    return evstore_free_bytes(out_free);
}

/* ═══ SD transfer: spool, reclaim, archive, re-import, legacy import ════════
 * Driven only by the keeper task (event_log_sd_keeper_start) — never the store
 * or claim hot path. File copies and verifications run WITHOUT s_mtx (rotated
 * files are immutable; the copied seq is pinned against eviction), inside one
 * sdcard_io_begin/end bracket each; s_mtx is taken only to decide and to append
 * index lines. With no card, or a parked/mismatched one, every step is a no-op:
 * measurement and publishing never notice. */

static void evq_keeper_notify(void)
{
    s_keeper_wake_pending = true;
    TaskHandle_t t = s_keeper_task;
    if (t != NULL) xTaskNotifyGive(t);
}

void event_log_sd_notify(void)
{
    /* Called by the SD monitor on EVERY mount-state transition: whatever card
     * is there now must be re-verified before its copies are trusted, even if
     * event_log itself never observed it unmounted (same CID, removed,
     * modified elsewhere, reinserted between two observations). */
    s_sd_notify_epoch++;
    evq_keeper_notify();
}

bool event_log_sd_service_pending(void)
{
    return s_keeper_wake_pending;
}

/* Per-service bookkeeping: did this pass write to the card (a "burst")? */
static bool s_pass_wrote_sd = false;
static bool s_pass_error    = false;

/* Bounded name copy: false (dst untouched) when src does not fit. Names we
 * create are short; a longer directory entry is simply not ours. */
static bool evq_name_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

static bool evq_name_join(char *dst, size_t cap, const char *base, const char *ext)
{
    size_t a = strlen(base), b = strlen(ext);
    if (a + b >= cap) return false;
    memcpy(dst, base, a);
    memcpy(dst + a, ext, b + 1);
    return true;
}

static bool evq_sd_path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Is the SD file a byte prefix of the flash file? (A crash-torn copy of our
 * own, which is safe to remove because the flash copy still holds it all.) */
static bool evq_is_prefix_of(const char *sd_path, const char *flash_path, char *buf, size_t cap)
{
    FILE *a = fopen(sd_path, "rb");
    if (a == NULL) return false;
    FILE *b = fopen(flash_path, "rb");
    if (b == NULL) { fclose(a); return false; }
    size_t half = cap / 2;
    bool prefix = true;
    for (;;) {
        size_t na = fread(buf, 1, half, a);
        if (na == 0) break;
        size_t nb = fread(buf + half, 1, na, b);
        if (nb != na || memcmp(buf, buf + half, na) != 0) { prefix = false; break; }
    }
    if (ferror(a) || ferror(b)) prefix = false;
    fclose(a);
    fclose(b);
    return prefix;
}

/* Stream `src` to `dir/<base>.log` via `dir/<base>[-k].tmp`: exclusive create,
 * copy, fflush+fsync+close (content-durable), rename (FatFs f_rename ends in
 * sync_fs: directory-durable on successful return), then reopen BY THE NEW
 * NAME and verify size/CRC/structure against `expect`. Caller holds an SD ref
 * and no s_mtx. `fp` prefixes the fault-point names. */
static bool evq_copy_commit(const char *src, const char *dir, const char *base,
                            const evq_seg_t *expect, const char *fp)
{
    char tmp[EVQ_PATH_MAX], dst[EVQ_PATH_MAX], name[64];
    snprintf(dst, sizeof dst, "%s/%s.log", dir, base);
    if (evq_sd_path_exists(dst)) return false;            /* never overwrite */
    snprintf(tmp, sizeof tmp, "%s/%s.tmp", dir, base);
    for (unsigned k = 1; evq_sd_path_exists(tmp) && k < 16; k++) {
        snprintf(tmp, sizeof tmp, "%s/%s-%u.tmp", dir, base, k);
    }
    if (evq_sd_path_exists(tmp)) return false;

    FILE *rf = fopen(src, "rb");
    if (rf == NULL) return false;
    FILE *wf = fopen(tmp, "wx");
    s_pass_wrote_sd = true;
    if (wf == NULL) { fclose(rf); return false; }
    snprintf(name, sizeof name, "%s.after_tmp_open", fp); EVQ_FAULT_POINT(name);
    bool ok = true;
    bool first = true;
    size_t n;
    while ((n = fread(s_kbuf, 1, s_line_cap, rf)) > 0) {
        if (fwrite(s_kbuf, 1, n, wf) != n) { ok = false; break; }
        if (first) { first = false; snprintf(name, sizeof name, "%s.mid_copy", fp); EVQ_FAULT_POINT(name); }
    }
    if (ferror(rf)) ok = false;
    fclose(rf);
    snprintf(name, sizeof name, "%s.before_tmp_fsync", fp); EVQ_FAULT_POINT(name);
    if (ok && (fflush(wf) != 0 || fsync(fileno(wf)) != 0)) ok = false;
    if (fclose(wf) != 0) ok = false;
    if (!ok) { remove(tmp); return false; }
    snprintf(name, sizeof name, "%s.after_tmp_fsync_before_rename", fp); EVQ_FAULT_POINT(name);
    snprintf(name, sizeof name, "%s.rename.inside_call", fp); EVQ_FAULT_POINT(name);
    if (rename(tmp, dst) != 0) { remove(tmp); return false; }
    snprintf(name, sizeof name, "%s.rename.after_return", fp); EVQ_FAULT_POINT(name);
    snprintf(name, sizeof name, "%s.after_rename_before_verify", fp); EVQ_FAULT_POINT(name);
    snprintf(name, sizeof name, "%s.verify_read_error", fp); EVQ_FAULT_POINT(name);
    evq_scan_t sc;
    if (!evq_scan_file(dst, s_kbuf, s_line_cap, &sc) || !evq_scan_matches(&sc, expect)) {
        ESP_LOGW(TAG, "SD copy %s failed read-back verification — removed", dst);
        remove(dst);
        return false;
    }
    return true;
}

/* Choose (and, where an identical copy already exists, adopt) an SD name for
 * seg `s` in `dir`. `cand` is the preferred basename (without .log); fallbacks
 * are generated by `fallback(k)`. Returns: 1 adopted existing verified copy,
 * 0 free name chosen in `out`, -1 no name available. Caller holds an SD ref. */
static int evq_pick_name(const char *dir, const char *flash_path, const evq_seg_t *s,
                         const char *cand, bool mirror, char *out, size_t cap)
{
    char path[EVQ_PATH_MAX];
    for (unsigned k = 0; k < 16; k++) {
        char base[48];
        if (k == 0) snprintf(base, sizeof base, "%s", cand);
        else if (mirror) snprintf(base, sizeof base, "m-%06u-%u", (unsigned)s->seq, k);
        else snprintf(base, sizeof base, "ev-%u", (unsigned)(EVQ_FALLBACK_NAME_BASE + s->seq + (k - 1U)));
        snprintf(path, sizeof path, "%s/%s.log", dir, base);
        if (!evq_sd_path_exists(path)) {
            snprintf(out, cap, "%s", base);
            return 0;
        }
        /* Taken. Our own intact copy from an interrupted pass → adopt; our own
         * crash-torn copy (a byte prefix of the flash file) → remove, reuse the
         * name; anything else is someone else's file → never touched. */
        evq_scan_t sc;
        if (evq_scan_file(path, s_kbuf, s_line_cap, &sc) && evq_scan_matches(&sc, s)) {
            snprintf(out, cap, "%s", base);
            EVQ_FAULT_POINT("boot.adopt_mid");
            return 1;
        }
        if (evq_is_prefix_of(path, flash_path, s_kbuf, s_line_cap)) {
            s_pass_wrote_sd = true;
            if (remove(path) == 0) { snprintf(out, cap, "%s", base); return 0; }
        }
    }
    return -1;
}

static bool evq_sd_has_room(uint64_t need)
{
    uint64_t freeb = 0;
    if (sdcard_free_bytes(&freeb) != ESP_OK) return false;
    return freeb > EVQ_SD_RESERVE_BYTES && freeb - EVQ_SD_RESERVE_BYTES >= need;
}

/* Spool one unsent FLASH segment: primary + mirror, both verified, then the
 * SPOOLED index line. The flash copy is kept; reclaim is a separate step. */
static bool evq_spool_one(uint32_t seq)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    evq_seg_t *sp = evq_index_find(&s_ix, seq);
    if (sp == NULL || sp->state != EVQ_SEG_FLASH || !sp->flash_present || !evq_sd_usable_locked() ||
        s_sd_mismatch_park) {
        xSemaphoreGive(s_mtx);
        return false;
    }
    evq_seg_t s = *sp;
    uint32_t cid = s_sd_last_cid;
    s_keeper_pin_seq = seq;
    xSemaphoreGive(s_mtx);

    char flash_path[EVQ_PATH_MAX];
    evlog_file_path(flash_path, sizeof flash_path, seq);
    bool ok = false, full = false;
    char pname[EVQ_NAME_MAX] = "", mname[EVQ_NAME_MAX] = "";
    if (sdcard_io_begin()) {
        mkdir(EVLOG_LEGACY_SD_DIR, 0777);
        mkdir(EVQ_MIRROR_DIR, 0777);
        char pcand[48], mcand[48], pbase[48], mbase[48];
        /* EXACTLY "ev-%06u": every released importer (v1.10.0–v2.4.2) rebuilds
         * the path as "%s/ev-%06u.log" from the parsed number, so any other
         * spelling of the same number is invisible to a rolled-back firmware. */
        snprintf(pcand, sizeof pcand, "ev-%06lld", (long long)s.first_id);
        snprintf(mcand, sizeof mcand, "m-%06u", (unsigned)seq);
        int pr = evq_pick_name(EVLOG_LEGACY_SD_DIR, flash_path, &s, pcand, false, pbase, sizeof pbase);
        int mr = pr >= 0 ? evq_pick_name(EVQ_MIRROR_DIR, flash_path, &s, mcand, true, mbase, sizeof mbase) : -1;
        uint64_t need = (uint64_t)s.bytes * (uint64_t)((pr == 0) + (mr == 0)) + 64 * 1024;
        if (pr < 0 || mr < 0) {
            ok = false;
        } else if (!evq_sd_has_room(need)) {
            full = true;
        } else {
            bool p_ok = pr == 1 || evq_copy_commit(flash_path, EVLOG_LEGACY_SD_DIR, pbase, &s, "spool");
            ok = p_ok && (mr == 1 || evq_copy_commit(flash_path, EVQ_MIRROR_DIR, mbase, &s, "mirror"));
            if (p_ok && !ok) {
                /* Our own verified primary without a mirror is not a SPOOLED copy;
                 * left in /sdcard/events it would later look like foreign legacy
                 * backlog and be re-imported (duplicates). The flash copy still
                 * holds everything, so remove it; the next pass starts over. */
                char pp[EVQ_PATH_MAX];
                snprintf(pp, sizeof pp, "%s/%s.log", EVLOG_LEGACY_SD_DIR, pbase);
                remove(pp);
            }
            if (!evq_name_join(pname, sizeof pname, pbase, ".log") ||
                !evq_name_join(mname, sizeof mname, mbase, ".log")) ok = false;
        }
        if (ok) sdcard_report_io_ok(); else if (!full) sdcard_report_io_error();
        sdcard_io_end();
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_keeper_pin_seq = 0;
    if (full) {
        if (!s_sd_full) ESP_LOGW(TAG, "SD below its %llu-B reserve — spooling paused", (unsigned long long)EVQ_SD_RESERVE_BYTES);
        s_sd_full = true;
        xSemaphoreGive(s_mtx);
        return false;
    }
    sp = evq_index_find(&s_ix, seq);
    if (ok && sp != NULL && sp->state == EVQ_SEG_FLASH && sp->crc == s.crc) {
        EVQ_FAULT_POINT("spool.after_verify_before_index");
        if (EVQ_IX_APPEND("P %" PRIu32 " %08" PRIx32 " %s %s", seq, cid, pname, mname) == ESP_OK) {
            EVQ_FAULT_POINT("spool.after_index_before_reclaim");
            s_spool_files++;
            sp = evq_index_find(&s_ix, seq);
            if (sp) { sp->sd_verified_epoch = s_sd_epoch; sp->sd_verified_which = 1; }
            xSemaphoreGive(s_mtx);
            return true;
        }
    }
    if (!ok) { s_pass_error = true; evq_sd_backoff_locked(); }
    xSemaphoreGive(s_mtx);
    return false;
}

/* Reclaim one SPOOLED segment's flash copy: re-verify BOTH SD copies first
 * (G6: a copy damaged after SPOOLED must never become the only copy), then
 * remove the flash file, then record SD_ONLY. */
static bool evq_reclaim_one(uint32_t seq)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    evq_seg_t *sp = evq_index_find(&s_ix, seq);
    if (sp == NULL || sp->state != EVQ_SEG_SPOOLED || !sp->flash_present || seq >= s_tail_seq ||
        !evq_sd_usable_locked() || s_sd_mismatch_park || sp->cid != s_sd_last_cid) {
        xSemaphoreGive(s_mtx);
        return false;
    }
    evq_seg_t s = *sp;
    xSemaphoreGive(s_mtx);

    bool ok = false;
    if (sdcard_io_begin()) {
        char pp[EVQ_PATH_MAX], mp[EVQ_PATH_MAX];
        evq_sd_primary_path(pp, sizeof pp, s.primary);
        evq_sd_mirror_path(mp, sizeof mp, s.mirror);
        evq_scan_t a, b;
        ok = evq_scan_file(pp, s_kbuf, s_line_cap, &a) && evq_scan_matches(&a, &s) &&
             evq_scan_file(mp, s_kbuf, s_line_cap, &b) && evq_scan_matches(&b, &s);
        sdcard_io_end();
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    sp = evq_index_find(&s_ix, seq);
    if (sp == NULL || sp->state != EVQ_SEG_SPOOLED || !sp->flash_present) { xSemaphoreGive(s_mtx); return false; }
    if (!ok) {
        /* A spooled copy went bad: back to FLASH (flash copy is the only one) —
         * the next burst re-spools under fresh names. */
        ESP_LOGW(TAG, "ev-%06u.log: SD copies fail re-verification — reclaim refused, will re-spool", (unsigned)seq);
        (void)EVQ_IX_APPEND("S %" PRIu32 " %lld %lld %" PRIu32 " %" PRIu32 " %08" PRIx32,
                            seq, (long long)s.first_id, (long long)s.last_id, s.count, s.bytes, s.crc);
        sp = evq_index_find(&s_ix, seq);
        if (sp) sp->flash_present = true;
        s_spool_errors++;
        xSemaphoreGive(s_mtx);
        return false;
    }
    char path[EVQ_PATH_MAX];
    evlog_file_path(path, sizeof path, seq);
    EVQ_FAULT_POINT("reclaim.remove.inside_call");
    if (remove(path) != 0) { xSemaphoreGive(s_mtx); return false; }
    sp->flash_present = false;
    EVQ_FAULT_POINT("reclaim.after_remove_before_index");
    (void)EVQ_IX_APPEND("O %" PRIu32, seq);
    EVQ_FAULT_POINT("reclaim.after_index");
    s_reclaimed_files++;
    xSemaphoreGive(s_mtx);
    return true;
}

/* Archive one DELIVERED segment (seq < cursor). A spooled one moves its primary
 * into the archive by rename (cheap metadata; FatFs: durable on return) and
 * drops the mirror; a never-spooled one is copied from flash. The index entry
 * retires (A) only after the archive name verifies, and the flash copy goes last. */
static bool evq_archive_one(uint32_t seq)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    evq_seg_t *sp = evq_index_find(&s_ix, seq);
    if (sp == NULL || seq >= s_rd_seq || sp->state == EVQ_SEG_REIMPORT || !evq_sd_usable_locked() ||
        s_sd_mismatch_park) {
        xSemaphoreGive(s_mtx);
        return false;
    }
    if (sp->primary[0] != '\0' && sp->cid != s_sd_last_cid) {
        /* Delivered, but its copies are on another card: obligation met, drop
         * the reference (the flash copy, if any, is left to eviction). */
        (void)EVQ_IX_APPEND("A %" PRIu32, seq);
        xSemaphoreGive(s_mtx);
        return true;
    }
    if (sp->state != EVQ_SEG_DELIVERED) (void)EVQ_IX_APPEND("D %" PRIu32, seq);
    sp = evq_index_find(&s_ix, seq);
    if (sp == NULL) { xSemaphoreGive(s_mtx); return false; }
    evq_seg_t s = *sp;
    s_keeper_pin_seq = seq;
    xSemaphoreGive(s_mtx);
    EVQ_FAULT_POINT("delivered.after_index_before_archive_rename");

    char flash_path[EVQ_PATH_MAX];
    evlog_file_path(flash_path, sizeof flash_path, seq);
    bool ok = false;
    if (sdcard_io_begin()) {
        mkdir(EVLOG_ARCHIVE_DIR, 0777);
        char base[64], dst[EVQ_PATH_MAX];
        /* Collision-free archive name: arc-<first_id>, then arc-<first_id>-<seq>,
         * then arc-<first_id>-<seq + k·10^8> (evlog_inventory parses one numeric
         * suffix). Never overwrite an archive. */
        base[0] = '\0';
        for (unsigned k = 0; k < 8; k++) {
            if (k == 0) snprintf(base, sizeof base, "arc-%lld", (long long)s.first_id);
            else snprintf(base, sizeof base, "arc-%lld-%u", (long long)s.first_id,
                          (unsigned)(s.seq + (k - 1U) * 100000000U));
            snprintf(dst, sizeof dst, "%s/%s.log", EVLOG_ARCHIVE_DIR, base);
            if (!evq_sd_path_exists(dst)) break;
            base[0] = '\0';
        }
        char pp[EVQ_PATH_MAX], mp[EVQ_PATH_MAX];
        evq_sd_primary_path(pp, sizeof pp, s.primary);
        evq_sd_mirror_path(mp, sizeof mp, s.mirror);
        /* Source order: the primary (renamed into place, cheap), else the
         * mirror, else the flash copy (both copied + verified under the new
         * name). The redundant copies (mirror, flash) are dropped ONLY after
         * a verified archive copy exists; a failed verification keeps them and
         * retries on a later pass (§1.8).
         *
         * Every SD source is verified IN PLACE first (eval R2-F1): a primary
         * whose bytes read back but differ from the index is conclusively
         * damaged. Renaming it into the archive and back on every pass
         * starved its healthy mirror and, through the transfer loop, all
         * later spooling (flash filled up to refusal with the SD mostly
         * empty). A damaged copy is therefore never a source: the archive
         * comes from the verified mirror/flash copy and the damaged bytes
         * move aside to evq/bad-* (A1), never deleted. An UNREADABLE copy
         * (I/O error, maybe transient) is retried with backoff for
         * EVQ_ARCHIVE_TRIES passes, then treated the same way. */
        unsigned tries = s_arch_fail_seq == seq ? s_arch_fail_n : 0;
        bool give_up = tries >= EVQ_ARCHIVE_TRIES;
        bool have_primary = s.primary[0] != '\0' && evq_sd_path_exists(pp);
        bool have_mirror = s.mirror[0] != '\0' && evq_sd_path_exists(mp);
        evq_scan_t psc, msc;
        bool p_read = have_primary && evq_scan_file(pp, s_kbuf, s_line_cap, &psc);
        bool p_good = p_read && evq_scan_matches(&psc, &s);
        bool use_primary = p_good && !give_up;
        bool p_bad = have_primary && !use_primary && (p_read || give_up);   /* conclusive, or out of retries */
        bool m_read = false, m_good = false;
        if (have_mirror && !use_primary) {
            m_read = evq_scan_file(mp, s_kbuf, s_line_cap, &msc);
            m_good = m_read && evq_scan_matches(&msc, &s);
        }
        bool m_bad = have_mirror && !use_primary && !m_good && (m_read || give_up);
        bool set_aside_primary = false, set_aside_mirror = false;
        if (base[0] == '\0') {
            ok = false;
        } else if (use_primary) {
            s_pass_wrote_sd = true;
            EVQ_FAULT_POINT("archive.rename.inside_call");
            ok = rename(pp, dst) == 0;
            EVQ_FAULT_POINT("archive.after_rename_before_verify");
            if (ok) {
                evq_scan_t sc;
                if (!evq_scan_file(dst, s_kbuf, s_line_cap, &sc) || !evq_scan_matches(&sc, &s)) {
                    /* Not a verified archive: put it back where the index says it
                     * is and keep every other copy. If the move back fails too,
                     * the next pass archives from the mirror/flash copy (the
                     * unverified file just stays in the archive directory). */
                    ESP_LOGW(TAG, "archive %s failed verification — kept primary/mirror, will retry", dst);
                    (void)rename(dst, pp);
                    ok = false;
                }
            }
        } else if (have_primary && !p_bad) {
            ok = false;          /* primary unreadable: retry (bounded) before giving up on it */
        } else if (m_good) {
            set_aside_primary = have_primary;
            if (!evq_sd_has_room((uint64_t)s.bytes + 64 * 1024)) {
                s_sd_full = true;
                ok = false;
            } else {
                ok = evq_copy_commit(mp, EVLOG_ARCHIVE_DIR, base, &s, "archive");
            }
        } else if (have_mirror && !m_bad) {
            ok = false;          /* mirror unreadable: retry (bounded) */
        } else if (s.flash_present && evlog_flash_exists(seq, NULL)) {
            set_aside_primary = have_primary;
            set_aside_mirror = have_mirror;
            /* A never-spooled file may still have copies from an interrupted
             * spool (crash between commit and the SPOOLED line). Once this entry
             * retires they would read as foreign legacy backlog: drop any that
             * are ours (verify against the index, or a byte prefix of flash). */
            char cand[EVQ_PATH_MAX];
            const char *cdirs[2] = { EVLOG_LEGACY_SD_DIR, EVQ_MIRROR_DIR };
            for (int ci = 0; ci < 2 && !have_primary && !have_mirror; ci++) {
                if (ci == 0) snprintf(cand, sizeof cand, "%s/ev-%06lld.log", cdirs[ci], (long long)s.first_id);
                else snprintf(cand, sizeof cand, "%s/m-%06u.log", cdirs[ci], (unsigned)seq);
                if (!evq_sd_path_exists(cand)) continue;
                evq_scan_t cs;
                if ((evq_scan_file(cand, s_kbuf, s_line_cap, &cs) && evq_scan_matches(&cs, &s)) ||
                    evq_is_prefix_of(cand, flash_path, s_kbuf, s_line_cap)) {
                    s_pass_wrote_sd = true;
                    remove(cand);
                }
            }
            if (!evq_sd_has_room((uint64_t)s.bytes + 64 * 1024)) {
                s_sd_full = true;
                ok = false;
            } else {
                ok = evq_copy_commit(flash_path, EVLOG_ARCHIVE_DIR, base, &s, "archive");
            }
        } else if (have_primary || have_mirror) {
            /* Delivered, and every remaining copy is damaged: nothing verified
             * can be archived. The delivery obligation is met; keep the bytes
             * aside and retire the entry instead of blocking the keeper. */
            ESP_LOGE(TAG, "ev-%06u.log: delivered, no verified copy left — damaged copies kept in %s/bad-*",
                     (unsigned)seq, EVQ_MIRROR_DIR);
            set_aside_primary = have_primary;
            set_aside_mirror = have_mirror;
            ok = true;
        } else {
            ok = true;       /* no copy left anywhere (flash evicted, SD copies gone): nothing to archive */
        }
        /* Damaged copies leave the import/mirror names only once the archive
         * (or retirement) is settled; a failed move keeps the entry for retry. */
        if (ok && set_aside_primary) {
            s_pass_wrote_sd = true;
            ok = evq_sd_move_aside(pp, seq);
        }
        if (ok && set_aside_mirror) {
            s_pass_wrote_sd = true;
            ok = evq_sd_move_aside(mp, seq);
        }
        if (ok) {
            EVQ_FAULT_POINT("archive.after_verify_before_mirror_remove");
            if (s.mirror[0] != '\0' && evq_sd_path_exists(mp)) {
                s_pass_wrote_sd = true;
                EVQ_FAULT_POINT("archive.mirror_remove.inside_call");
                remove(mp);
            }
            EVQ_FAULT_POINT("archive.after_mirror_remove_before_index");
        }
        if (ok) sdcard_report_io_ok();
        sdcard_io_end();
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_keeper_pin_seq = 0;
    if (!ok) {
        if (s_arch_fail_seq == seq) { if (s_arch_fail_n < 255) s_arch_fail_n++; }
        else { s_arch_fail_seq = seq; s_arch_fail_n = 1; }
        if (!s_sd_full) { s_pass_error = true; evq_sd_backoff_locked(); }
        xSemaphoreGive(s_mtx);
        return false;
    }
    if (s_arch_fail_seq == seq) { s_arch_fail_seq = 0; s_arch_fail_n = 0; }
    (void)EVQ_IX_APPEND("A %" PRIu32, seq);
    s_archived_files++;
    /* The flash copy is delivered AND archived: free it now. */
    if (seq < s_rd_seq && evlog_flash_exists(seq, NULL)) remove(flash_path);
    xSemaphoreGive(s_mtx);
    return true;
}

/* Quarantine raw bytes that are not a valid record (import paths). */
static bool evq_quarantine_bytes_locked(const char *buf, size_t len)
{
    FILE *qf = fopen(EVLOG_QUARANTINE, "a");
    if (qf == NULL) return false;
    bool ok = fwrite(buf, 1, len, qf) == len;
    if (ok && buf[len - 1] != '\n') ok = fwrite("\n", 1, 1, qf) == 1;
    if (ok && (fflush(qf) != 0 || fsync(fileno(qf)) != 0)) ok = false;
    if (fclose(qf) != 0) ok = false;
    return ok;
}

/* Append one already-framed v2 record line to the tail exactly as stored on
 * another medium (legacy SD import, re-import, archive replay). Caller holds
 * s_mtx and `line` must NOT alias s_line. Gate: exactly what parse_record
 * accepts → ESP_ERR_INVALID_ARG otherwise (caller quarantines it).
 * ESP_ERR_NO_MEM = store full after evicting delivered copies (nothing was
 * written; the caller may retry later). ESP_FAIL = media write failure (the
 * torn partial is truncated away). On success *out_id is the record's id, the
 * record is PENDING (durable after the caller's flush), and next_id is kept
 * above it. */
static esp_err_t evlog_append_verbatim_locked(const char *line, size_t len, int64_t *out_id)
{
    if (len > s_max_record || !evq_line_is_valid(line, len)) return ESP_ERR_INVALID_ARG;
    int64_t id = (int64_t)strtoll(line, NULL, 10);
    if (s_wf == NULL && evlog_reopen_tail_locked() != ESP_OK) return ESP_ERR_INVALID_STATE;

    uint64_t freeb = 0;
    if (evstore_free_bytes(&freeb) == ESP_OK && freeb < EVLOG_MIN_FREE_BYTES) {
        (void)evlog_evict_synced_locked();
        if (evstore_free_bytes(&freeb) == ESP_OK && freeb < EVLOG_MIN_FREE_BYTES) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_tail_size > 0 && s_tail_size + (long)len > EVLOG_ROTATE_BYTES) {
        if (!evlog_rotate_locked()) return ESP_FAIL;
    }
    if (fwrite(line, 1, len, s_wf) != len) {
        fflush(s_wf);
        (void)ftruncate(fileno(s_wf), s_tail_size);
        return ESP_FAIL;
    }
    const char *parts[1] = { line };
    const size_t lens[1] = { len };
    evlog_tail_account(parts, lens, 1, id);
    s_tail_size += (long)len;
    evlog_bump_next_id_locked(id);
    if (out_id) *out_id = id;
    return ESP_OK;
}

esp_err_t event_log_append_verbatim(const char *line, size_t len, int64_t *out_id)
{
    if (line == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = !s_available ? ESP_ERR_INVALID_STATE
                                 : evlog_append_verbatim_locked(line, len, out_id);
    xSemaphoreGive(s_mtx);
    return err;
}

esp_err_t event_log_flush(void)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = evlog_flush_writer_locked();
    xSemaphoreGive(s_mtx);
    return err;
}

/* Re-append a REIMPORT segment's records (from its recorded offset) out of a
 * verified SD copy into the flash tail, fsync, THEN remove the SD copies, THEN
 * retire the entry. A crash anywhere before the retire replays the file from
 * the recorded offset: bounded duplicates, never a loss. */
static bool evq_reimport_one(uint32_t seq)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    evq_seg_t *sp = evq_index_find(&s_ix, seq);
    /* Room for THIS file above the refusal watermark is enough: requiring the
     * 40 % reclaim target here could never pass while the REIMPORT sources
     * themselves fill flash (their copies are exempt from eviction). Delivered
     * flash copies are given up first to make that room. */
    uint64_t need = sp != NULL ? (uint64_t)sp->bytes + EVLOG_MIN_FREE_BYTES + 4096U : 0;
    uint64_t freeb = 0;
    (void)evstore_free_bytes(&freeb);
    if (sp != NULL && freeb <= need) {
        (void)evlog_evict_to_locked(need + 1);
        (void)evstore_free_bytes(&freeb);
    }
    bool room = sp != NULL && freeb > need;
    bool flash_src = sp != NULL && evlog_flash_exists(seq, NULL);
    if (sp == NULL || sp->state != EVQ_SEG_REIMPORT || !room ||
        (!flash_src && (!evq_sd_usable_locked() || s_sd_mismatch_park || sp->cid != s_sd_last_cid))) {
        xSemaphoreGive(s_mtx);
        return false;
    }
    evq_seg_t s = *sp;
    if (flash_src) s_keeper_pin_seq = seq;
    xSemaphoreGive(s_mtx);

    /* Pick a verified copy: the flash file when it survived, else SD. */
    char src[EVQ_PATH_MAX] = "", pp[EVQ_PATH_MAX], mp[EVQ_PATH_MAX], fp[EVQ_PATH_MAX];
    evq_sd_primary_path(pp, sizeof pp, s.primary);
    evq_sd_mirror_path(mp, sizeof mp, s.mirror);
    evlog_file_path(fp, sizeof fp, seq);
    int64_t remaining = 0;
    evq_scan_t sc;
    if (flash_src && evq_scan_file(fp, s_kbuf, s_line_cap, &sc) && evq_scan_matches(&sc, &s)) {
        snprintf(src, sizeof src, "%s", fp);
    } else if (s.primary[0] != '\0') {
        if (!sdcard_io_begin()) { s_keeper_pin_seq = 0; return false; }
        if (evq_scan_file(pp, s_kbuf, s_line_cap, &sc) && evq_scan_matches(&sc, &s)) snprintf(src, sizeof src, "%s", pp);
        else if (s.mirror[0] && evq_scan_file(mp, s_kbuf, s_line_cap, &sc) && evq_scan_matches(&sc, &s)) snprintf(src, sizeof src, "%s", mp);
        sdcard_io_end();
        flash_src = false;
    }
    if (src[0] == '\0') {
        s_keeper_pin_seq = 0;
        ESP_LOGW(TAG, "ev-%06u.log owed for re-import but no verified SD copy is present — kept outstanding",
                 (unsigned)seq);
        return false;
    }

    long pos = (long)s.reimport_off;
    bool ok = true, done = false;
    while (ok && !done) {
        /* Read a chunk of whole lines with the SD ref only… */
        size_t used = 0, nlines = 0;
        long next = pos;
        if (!flash_src && !sdcard_io_begin()) { ok = false; break; }
        FILE *f = fopen(src, "rb");
        if (f == NULL || fseek(f, pos, SEEK_SET) != 0) { if (f) fclose(f); if (!flash_src) sdcard_io_end(); ok = false; break; }
        for (;;) {
            if (used + s_max_record + 1 > s_line_cap) break;
            if (fgets(s_kbuf + used, (int)(s_line_cap - used), f) == NULL) { done = true; break; }
            size_t l = strlen(s_kbuf + used);
            if (l == 0) { done = true; break; }
            used += l;
            nlines++;
            next += (long)l;
        }
        fclose(f);
        if (!flash_src) sdcard_io_end();
        if (nlines == 0) break;
        /* …then append them under s_mtx (lock order: never s_mtx inside a ref). */
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        size_t off = 0;
        while (off < used) {
            char *line = s_kbuf + off;
            size_t l = strlen(line);
            esp_err_t e = evlog_append_verbatim_locked(line, l, NULL);
            if (e == ESP_ERR_INVALID_ARG) {
                if (!evq_quarantine_bytes_locked(line, l)) { ok = false; break; }
                s_skipped++;
                if (evq_pos_before_watermark(seq, pos)) s_quarantined_malformed++; else s_corrupt_detected++;
            } else if (e != ESP_OK) { ok = false; break; }
            off += l;
            remaining++;
            if (remaining == 1) EVQ_FAULT_POINT("reimport.mid_append");
        }
        if (ok && evlog_flush_writer_locked() != ESP_OK) ok = false;
        xSemaphoreGive(s_mtx);
        pos = next;
    }
    s_keeper_pin_seq = 0;
    if (!ok) return false;
    EVQ_FAULT_POINT("reimport.after_fsync_before_sd_remove");
    if (s.primary[0] != '\0' && sdcard_io_begin()) {
        EVQ_FAULT_POINT("reimport.remove.inside_call");
        if (s.primary[0] && evq_sd_path_exists(pp)) remove(pp);
        if (s.mirror[0] && evq_sd_path_exists(mp)) remove(mp);
        s_pass_wrote_sd = true;
        sdcard_io_end();
    }
    EVQ_FAULT_POINT("reimport.after_sd_remove_before_index");
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    (void)EVQ_IX_APPEND("A %" PRIu32, seq);
    s_reimported_files++;
    /* A surviving flash copy is off-queue now (behind the cursor, re-appended). */
    if (seq < s_rd_seq && evlog_flash_exists(seq, NULL)) remove(fp);
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "re-imported ev-%06u.log (%lld record(s)) into the flash queue", (unsigned)seq, (long long)remaining);
    return true;
}

/* Is `name` referenced by the index (as a primary), or the preferred primary
 * name of a not-yet-spooled FLASH segment (the spool step owns those)? */
static bool evq_name_is_owned_locked(const char *name)
{
    for (size_t i = 0; i < s_ix.n; i++) {
        const evq_seg_t *s = &s_ix.segs[i];
        if (strcmp(s->primary, name) == 0) return true;
        if (s->state == EVQ_SEG_FLASH) {
            char cand[48];
            snprintf(cand, sizeof cand, "ev-%06lld.log", (long long)s->first_id);
            if (strcmp(cand, name) == 0) return true;
        }
    }
    return false;
}

/* Count complete lines in every UNOWNED /sdcard/events file (the legacy /
 * orphan import backlog). Called by the keeper with s_mtx held on a usable,
 * non-parked card. */
static void evq_count_legacy_locked(void)
{
    int64_t lines = 0;
    if (!sdcard_io_begin()) return;
    DIR *d = opendir(EVLOG_LEGACY_SD_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            uint32_t seq;
            if (!parse_ev_name(ent->d_name, &seq) || evq_name_is_owned_locked(ent->d_name)) continue;
            char p[EVQ_PATH_MAX];
            if (snprintf(p, sizeof p, "%s/%s", EVLOG_LEGACY_SD_DIR, ent->d_name) >= (int)sizeof p) continue;
            evq_scan_t sc;
            if (evq_scan_file(p, s_kbuf, s_line_cap, &sc)) lines += sc.count;
        }
        closedir(d);
    }
    sdcard_io_end();
    s_legacy_pending = lines;
    s_legacy_known = true;
}

/* Legacy / orphan import: re-append records from an UNOWNED /sdcard/events file
 * (pre-internal-store backlog, a rollback-era leftover, or an index-lost spool)
 * through the normal internal writer, verbatim (measure_id kept). Files import
 * oldest-first; each SD file is deleted only after its records are appended AND
 * fsync'd internally. Malformed lines are quarantined (intact) first. Imports at
 * most max_files per call; returns files imported. */
static size_t event_log_import_sd_backlog(size_t max_files)
{
    size_t files_done = 0;

    while (files_done < max_files) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (!s_available || !evq_sd_usable_locked() || s_sd_mismatch_park) { xSemaphoreGive(s_mtx); break; }
        if (s_wf == NULL && evlog_reopen_tail_locked() != ESP_OK) { xSemaphoreGive(s_mtx); break; }

        uint32_t min_seq = 0;
        bool found = false;
        char chosen[64] = "";
        if (sdcard_io_begin()) {
            DIR *d = opendir(EVLOG_LEGACY_SD_DIR);
            if (d != NULL) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    uint32_t seq;
                    if (!parse_ev_name(ent->d_name, &seq)) continue;
                    if (evq_name_is_owned_locked(ent->d_name)) continue;
                    if ((!found || seq < min_seq) && evq_name_copy(chosen, sizeof chosen, ent->d_name)) {
                        min_seq = seq; found = true;
                    }
                }
                closedir(d);
            }
            sdcard_io_end();
        }
        if (!found) { xSemaphoreGive(s_mtx); break; }

        char src[EVQ_PATH_MAX];
        snprintf(src, sizeof src, "%s/%s", EVLOG_LEGACY_SD_DIR, chosen);

        bool file_ok = true, store_full = false;
        size_t imported = 0, quarantined = 0;
        if (sdcard_io_begin()) {
            FILE *rf = fopen(src, "rb");
            if (rf == NULL) {
                file_ok = false;
            } else {
                EVQ_FAULT_POINT("import.mid_append");
                while (fgets(s_kbuf, s_line_cap, rf) != NULL) {
                    size_t len = strlen(s_kbuf);
                    if (len == s_line_cap - 1 && s_kbuf[len - 1] != '\n') {
                        /* over-long: quarantine this chunk and the rest of the line */
                        if (!evq_quarantine_bytes_locked(s_kbuf, len)) { file_ok = false; break; }
                        int c;
                        while ((c = fgetc(rf)) != EOF && c != '\n') {}
                        quarantined++;
                        continue;
                    }
                    int64_t id = 0;
                    esp_err_t aerr = evlog_append_verbatim_locked(s_kbuf, len, &id);
                    if (aerr == ESP_ERR_INVALID_ARG) {
                        if (!evq_quarantine_bytes_locked(s_kbuf, len)) { file_ok = false; break; }
                        quarantined++;
                        continue;
                    }
                    if (aerr == ESP_ERR_NO_MEM) { store_full = true; break; }   /* remainder stays on SD */
                    if (aerr != ESP_OK) { file_ok = false; break; }
                    imported++;
                }
                if (ferror(rf)) file_ok = false;
                fclose(rf);
            }
            /* Delete the SD source only when its every record is durably internal. */
            if (file_ok && !store_full && evlog_flush_writer_locked() == ESP_OK) {
                EVQ_FAULT_POINT("import.after_fsync_before_sd_remove");
                remove(src);
                s_pass_wrote_sd = true;
                files_done++;
                s_quarantined_malformed += (int64_t)quarantined;
                s_skipped += (int64_t)quarantined;
                int64_t done_lines = (int64_t)(imported + quarantined);
                s_legacy_pending = s_legacy_pending > done_lines ? s_legacy_pending - done_lines : 0;
            } else {
                file_ok = false;
            }
            sdcard_io_end();
        } else {
            file_ok = false;
        }

        if (imported > 0 || quarantined > 0) {
            ESP_LOGI(TAG, "migrated %s: %u record(s) imported, %u quarantined%s",
                     src, (unsigned)imported, (unsigned)quarantined,
                     store_full ? " (store full — resuming later)" : "");
        }
        xSemaphoreGive(s_mtx);
        if (!file_ok || store_full) break;
        vTaskDelay(1);
    }
    return files_done;
}

/* Once per SD mount epoch (card present, not parked/mismatched): clear stale
 * .tmp files (never the only copy — the flash copy is kept until SPOOLED), and
 * turn orphan mirrors (no index entry, e.g. after a lost index) into import
 * candidates. A .tmp whose .log sibling also exists may be the "both names"
 * outcome of an interrupted f_rename — on FAT those two entries can share one
 * cluster chain, so unlinking the .tmp would free the .log's clusters. It is
 * RETIRED (renamed aside, never unlinked) instead. */
static bool evq_sd_epoch_repair(void)
{
    bool more = false;             /* a bounded batch was full: repeat next pass */
    if (!sdcard_io_begin()) return true;
    mkdir(EVLOG_LEGACY_SD_DIR, 0777);
    mkdir(EVQ_MIRROR_DIR, 0777);
    mkdir(EVLOG_ARCHIVE_DIR, 0777);
    const char *dirs[3] = { EVLOG_LEGACY_SD_DIR, EVQ_MIRROR_DIR, EVLOG_ARCHIVE_DIR };
    unsigned retired = 0;
    for (int di = 0; di < 3; di++) {
        DIR *d = opendir(dirs[di]);
        if (d == NULL) continue;
        char names[32][64];
        int nn = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && nn < 32) {
            size_t l = strlen(ent->d_name);
            if (l > 4 && l < 60 && strcmp(ent->d_name + l - 4, ".tmp") == 0) {
                if (evq_name_copy(names[nn], sizeof names[nn], ent->d_name)) nn++;
            }
        }
        closedir(d);
        if (nn == 32) more = true;
        for (int i = 0; i < nn; i++) {
            /* Siblings: "<X>.tmp" pairs with "<X>.log"; an alternate
             * "<X>-<k>.tmp" pairs with "<X>.log" too. */
            char tmp[EVQ_PATH_MAX], log1[EVQ_PATH_MAX], log2[EVQ_PATH_MAX], base[64];
            snprintf(tmp, sizeof tmp, "%s/%s", dirs[di], names[i]);
            snprintf(base, sizeof base, "%s", names[i]);
            base[strlen(base) - 4] = '\0';
            snprintf(log1, sizeof log1, "%s/%s.log", dirs[di], base);
            log2[0] = '\0';
            char *dash = strrchr(base, '-');
            if (dash != NULL && dash[1] != '\0' && strspn(dash + 1, "0123456789") == strlen(dash + 1)) {
                *dash = '\0';
                snprintf(log2, sizeof log2, "%s/%s.log", dirs[di], base);
            }
            s_pass_wrote_sd = true;
            if (evq_sd_path_exists(log1) || (log2[0] != '\0' && evq_sd_path_exists(log2))) {
                char junk[EVQ_PATH_MAX];
                for (unsigned k = 0; k < 1000; k++) {
                    snprintf(junk, sizeof junk, "%s/xlk-%u.junk", EVQ_MIRROR_DIR, k);
                    if (!evq_sd_path_exists(junk)) break;
                }
                if (rename(tmp, junk) == 0) retired++;
            } else {
                remove(tmp);
            }
        }
    }
    /* Observable, bounded policy (contract amendment A1): retired names stay
     * on the card for fsck/inspection and are counted each mount; at most
     * 1000 slots — past that a .tmp is simply left in place (a .tmp is never
     * a committed copy and no firmware imports it). */
    unsigned on_card = 0, bad_on_card = 0;
    {
        DIR *d = opendir(EVQ_MIRROR_DIR);
        if (d != NULL) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strncmp(ent->d_name, "xlk-", 4) == 0) on_card++;
                else if (strncmp(ent->d_name, "bad-", 4) == 0) bad_on_card++;
            }
            closedir(d);
        }
    }
    s_sd_retired_names = on_card;
    if (bad_on_card > s_sd_bad_copies) s_sd_bad_copies = bad_on_card;
    sdcard_io_end();
    if (retired > 0) ESP_LOGW(TAG, "retired %u possibly cross-linked .tmp name(s) (not unlinked; %u on card)", retired, on_card);

    /* Orphan mirrors → import candidates (only if their primary is absent). */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (sdcard_io_begin()) {
        DIR *d = opendir(EVQ_MIRROR_DIR);
        char orphans[16][40];
        int no = 0;
        if (d != NULL) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL && no < 16) {
                const char *n = ent->d_name;
                size_t l = strlen(n);
                if (strncmp(n, "m-", 2) != 0 || l < 7 || strcmp(n + l - 4, ".log") != 0 || l >= 40) continue;
                bool owned = false;
                for (size_t i = 0; i < s_ix.n && !owned; i++) {
                    if (strcmp(s_ix.segs[i].mirror, n) == 0) owned = true;
                    if (s_ix.segs[i].state == EVQ_SEG_FLASH && (uint32_t)strtoul(n + 2, NULL, 10) == s_ix.segs[i].seq) owned = true;
                }
                if (!owned && evq_name_copy(orphans[no], sizeof orphans[no], n)) no++;
            }
            closedir(d);
        }
        if (no == 16) more = true;
        for (int i = 0; i < no; i++) {
            char mp[EVQ_PATH_MAX];
            evq_sd_mirror_path(mp, sizeof mp, orphans[i]);
            FILE *f = fopen(mp, "rb");
            long long first = 0;
            if (f != NULL) { if (fgets(s_kbuf, 64, f) != NULL) first = strtoll(s_kbuf, NULL, 10); fclose(f); }
            if (first <= 0) continue;
            char pp[EVQ_PATH_MAX];
            snprintf(pp, sizeof pp, "%s/ev-%06lld.log", EVLOG_LEGACY_SD_DIR, first);
            s_pass_wrote_sd = true;
            if (evq_sd_path_exists(pp)) {
                remove(mp);                            /* its primary will be imported */
            } else {
                for (unsigned k = 0; k < 16 && evq_sd_path_exists(pp); k++) {
                    snprintf(pp, sizeof pp, "%s/ev-%u.log", EVLOG_LEGACY_SD_DIR, (unsigned)(EVQ_FALLBACK_NAME_BASE + 500000000U + k));
                }
                (void)rename(mp, pp);                  /* becomes a legacy-import candidate */
            }
        }
        sdcard_io_end();
    }
    xSemaphoreGive(s_mtx);
    return more;
}

/* Index rotated flash files that have no entry (pre-upgrade files, or a crash
 * between rotation and its S line). Flash-only; bounded per pass. */
static void evq_index_unindexed(size_t max_files)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t min_seq = 0, max_seq = 0;
    bool any = evlog_scan_range_locked(&min_seq, &max_seq);
    uint32_t tail = s_tail_seq;
    xSemaphoreGive(s_mtx);
    if (!any || !s_ix_ready) return;

    size_t done = 0;
    bool remaining = false;
    int64_t floor_count = 0;
    for (uint32_t seq = min_seq; seq < tail; seq++) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        bool need = evq_index_find(&s_ix, seq) == NULL && evlog_flash_exists(seq, NULL);
        bool at_after_cursor = seq >= s_rd_seq;
        if (need) s_keeper_pin_seq = seq;
        xSemaphoreGive(s_mtx);
        if (!need) continue;
        if (done >= max_files) { remaining = remaining || at_after_cursor; continue; }
        char path[EVQ_PATH_MAX];
        evlog_file_path(path, sizeof path, seq);
        evq_scan_t sc;
        bool ok = evq_scan_file(path, s_kbuf, s_line_cap, &sc);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_keeper_pin_seq = 0;
        if (ok && evq_index_find(&s_ix, seq) == NULL && sc.count > 0) {
            if (EVQ_IX_APPEND("S %" PRIu32 " %lld %lld %" PRIu32 " %" PRIu32 " %08" PRIx32,
                              seq, (long long)sc.first_id, (long long)sc.last_id, sc.count, sc.bytes, sc.crc) == ESP_OK) {
                evq_seg_t *s = evq_index_find(&s_ix, seq);
                if (s) { s->flash_present = true; s->flash_crc_checked = 1; }
                evlog_bump_next_id_locked(sc.last_id);
                EVQ_FAULT_POINT("boot.index_rebuild_mid");
            } else if (at_after_cursor) {
                remaining = true;
                floor_count += sc.count;
            }
        } else if (ok && sc.count == 0 && seq < s_rd_seq) {
            /* empty delivered file: nothing to index */
        } else if (at_after_cursor) {
            remaining = true;
            if (ok) floor_count += sc.count;
        }
        xSemaphoreGive(s_mtx);
        done++;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_unidx_present = remaining;
    s_unidx_floor = remaining ? floor_count : 0;
    xSemaphoreGive(s_mtx);
}

/* Bounded pending floor for unindexed files still waiting (C9). */
static void evq_unindexed_floor_refresh(void)
{
    if (!s_unidx_present) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int64_t lines = 0;
    const TickType_t t0 = xTaskGetTickCount();
    bool capped = false;
    for (uint32_t seq = s_rd_seq; seq < s_tail_seq && !capped; seq++) {
        if (evq_index_find(&s_ix, seq) != NULL) continue;
        char path[EVQ_PATH_MAX];
        evlog_file_path(path, sizeof path, seq);
        FILE *f = fopen(path, "rb");
        if (f == NULL) continue;
        while (fgets(s_line, s_line_cap, f) != NULL) {
            size_t len = strlen(s_line);
            if (len == 0 || s_line[len - 1] != '\n') break;
            if (++lines >= EVLOG_SCAN_MAX_LINES ||
                (xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(EVLOG_SCAN_MAX_MS)) { capped = true; break; }
        }
        fclose(f);
    }
    s_unidx_floor = lines;
    xSemaphoreGive(s_mtx);
}

esp_err_t event_log_sd_service(void)
{
    if (s_mtx == NULL || !s_available) return ESP_ERR_INVALID_STATE;
    s_keeper_wake_pending = false;
    s_pass_wrote_sd = false;
    s_pass_error = false;

    /* 1. Flash-only bookkeeping: index unindexed rotated files. */
    evq_index_unindexed(8);
    evq_unindexed_floor_refresh();
    if (s_kbuf == NULL) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool usable = evq_sd_usable_locked() && !s_sd_mismatch_park && evq_sd_backoff_elapsed();
    bool repair = usable && s_sd_repaired_epoch != s_sd_epoch;
    uint32_t cid = s_sd_last_cid;
    bool due = s_stores_since_archive >= EVLOG_ARCHIVE_EVERY_N;
    uint64_t freeb = 0;
    bool pressure = evq_pressure_locked(&freeb);
    /* Adoption of a new card: every obligation elsewhere is SPOOLED (flash
     * copy still verified) or delivered — re-spool from flash, forget the rest. */
    if (usable) {
        for (size_t i = 0; i < s_ix.n;) {
            evq_seg_t *s = &s_ix.segs[i];
            uint32_t seq = s->seq;
            if (s->primary[0] != '\0' && s->cid != cid && s->state == EVQ_SEG_SPOOLED && s->flash_present) {
                ESP_LOGW(TAG, "ev-%06u.log was spooled to card %08" PRIx32 " — re-spooling to %08" PRIx32,
                         (unsigned)seq, s->cid, cid);
                (void)EVQ_IX_APPEND("S %" PRIu32 " %lld %lld %" PRIu32 " %" PRIu32 " %08" PRIx32,
                                    seq, (long long)s->first_id, (long long)s->last_id, s->count, s->bytes, s->crc);
                due = true;   /* re-spool promptly */
            } else if (s->primary[0] != '\0' && s->cid != cid && s->state == EVQ_SEG_DELIVERED) {
                (void)EVQ_IX_APPEND("A %" PRIu32, seq);
                continue;
            }
            s = evq_index_find(&s_ix, seq);
            if (s == NULL) continue;
            i++;
        }
    }
    xSemaphoreGive(s_mtx);

    if (repair) {
        bool more = evq_sd_epoch_repair();
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (!more) s_sd_repaired_epoch = s_sd_epoch;
        else s_legacy_known = false;          /* orphans not all visited yet: pending is a floor */
        xSemaphoreGive(s_mtx);
    }

    /* 2. Recovery: re-import obligations that left the queue unACKed (from a
     *    surviving flash copy even with no card; else from this card). */
    {
        uint32_t cand[32];
        size_t nc = 0;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        for (size_t i = 0; i < s_ix.n && nc < 32; i++) {
            const evq_seg_t *s = &s_ix.segs[i];
            if (s->state == EVQ_SEG_REIMPORT && (evlog_flash_exists(s->seq, NULL) || (usable && s->cid == cid))) {
                cand[nc++] = s->seq;
            }
        }
        xSemaphoreGive(s_mtx);
        for (size_t k = 0; k < nc; k++) (void)evq_reimport_one(cand[k]);   /* each tried once per pass */
    }

    if (usable) {

        /* Recovery work (re-import / legacy import) needs flash room; delivered
         * files still held for the archive are the first space to give up. */
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        bool import_work = s_legacy_pending > 0;
        for (size_t i = 0; i < s_ix.n && !import_work; i++) import_work = s_ix.segs[i].state == EVQ_SEG_REIMPORT;
        uint64_t fb0 = 0, tot0 = 0;
        (void)evstore_space(&fb0, &tot0);
        bool short_room = tot0 > 0 && fb0 * 100U < tot0 * EVQ_RECLAIM_PCT;
        xSemaphoreGive(s_mtx);

        /* 3. Transfer burst: batch trigger, flash pressure, or recovery needing room. */
        if (due || pressure || (import_work && short_room)) {
            /* Unsent records first (eval R2-F1): spooling is the obligation
             * that keeps flash from filling; archiving delivered files is
             * housekeeping, so a stuck archive can never starve the spool. */
            bool clean = true;
            for (size_t guard = 0; guard < EVQ_INDEX_CAP; guard++) {
                uint32_t seq = 0;
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                for (size_t i = 0; i < s_ix.n; i++) {
                    const evq_seg_t *s = &s_ix.segs[i];
                    if (s->state == EVQ_SEG_FLASH && s->flash_present && s->seq >= s_rd_seq && s->seq < s_tail_seq) { seq = s->seq; break; }
                }
                xSemaphoreGive(s_mtx);
                if (seq == 0) break;
                if (!evq_spool_one(seq)) { clean = false; break; }
            }
            /* A spool that stopped on a full card still lets delivered files
             * archive (their flash copies are the space recovery gives up
             * first); an I/O error backs the whole card off. */
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            bool arch_go = clean || (s_sd_full && evq_sd_backoff_elapsed());
            xSemaphoreGive(s_mtx);
            for (size_t guard = 0; guard < EVQ_INDEX_CAP && arch_go; guard++) {
                uint32_t seq = 0;
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                for (size_t i = 0; i < s_ix.n; i++) {
                    const evq_seg_t *s = &s_ix.segs[i];
                    if (s->seq < s_rd_seq && s->state != EVQ_SEG_REIMPORT) { seq = s->seq; break; }
                }
                xSemaphoreGive(s_mtx);
                if (seq == 0) break;
                if (!evq_archive_one(seq)) { clean = false; break; }
            }
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            if (clean) { s_stores_since_archive = 0; s_sd_full = false; }
            xSemaphoreGive(s_mtx);
        }

        /* 4. Reclaim verified flash copies oldest-first under pressure. */
        if (pressure) {
            for (size_t guard = 0; guard < EVQ_INDEX_CAP; guard++) {
                uint64_t fb = 0, tot = 0;
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                (void)evstore_space(&fb, &tot);
                uint32_t seq = 0;
                if (tot > 0 && fb * 100U < tot * EVQ_RECLAIM_PCT) {
                    for (size_t i = 0; i < s_ix.n; i++) {
                        const evq_seg_t *s = &s_ix.segs[i];
                        if (s->state == EVQ_SEG_SPOOLED && s->flash_present && s->seq < s_tail_seq) { seq = s->seq; break; }
                    }
                }
                xSemaphoreGive(s_mtx);
                if (seq == 0 || !evq_reclaim_one(seq)) break;
            }
        }

        /* 5. Legacy / orphan import (pre-existing behaviour), when flash has room. */
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        uint64_t fb = 0, tot = 0;
        (void)evstore_space(&fb, &tot);
        bool import_room = tot > 0 && fb * 100U >= tot * EVQ_RECLAIM_PCT;
        xSemaphoreGive(s_mtx);
        if (import_room) (void)event_log_import_sd_backlog(4);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (evq_sd_usable_locked() && !s_sd_mismatch_park) {
            /* Full scan once per mount (and until the repair pass has visited
             * every orphan); imports then decrement it, so a large legacy
             * backlog is not re-read every keeper period. */
            if (s_legacy_counted_epoch != s_sd_epoch || !s_legacy_known) {
                evq_count_legacy_locked();
                s_legacy_counted_epoch = s_sd_epoch;
            }
            if (s_sd_repaired_epoch != s_sd_epoch) s_legacy_known = false;
        }
        xSemaphoreGive(s_mtx);
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_pass_wrote_sd) s_sd_bursts++;
    if (!s_pass_error && usable) { s_sd_backoff_active = false; s_sd_backoff_ms = 0; }
    /* Space may have come back: let the next store re-check admission. */
    evq_index_maybe_compact_locked();
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

static void evq_keeper_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EVQ_KEEPER_PERIOD_MS));
        (void)event_log_sd_service();
    }
}

esp_err_t event_log_sd_keeper_start(void)
{
    if (s_keeper_task != NULL) return ESP_OK;
    if (s_mtx == NULL || !s_available) return ESP_ERR_INVALID_STATE;
    /* File copy/verify loops + VFS/FATFS call depth, plus the epoch repair's
     * name tables (~2.7 KB on stack): 8 KB, up from the old keeper's 6 KB.
     * No mount fan-out. Priority 2 with the other background housekeeping. */
    if (xTaskCreate(evq_keeper_task, "sd_keeper", 8192, NULL, 2, &s_keeper_task) != pdPASS) {
        s_keeper_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (s_keeper_wake_pending) xTaskNotifyGive(s_keeper_task);
    return ESP_OK;
}

/* ── archive-replay support (evlog_replay) ─────────────────────────────────
 * Both walk the QUEUE (flash or verified SD copy). An unreadable SD segment
 * makes the answer incomplete (*out_capped), never silently smaller. */

esp_err_t event_log_max_pending_id_in_range(int64_t from_id, int64_t to_id,
                                            int64_t *out_max, bool *out_capped)
{
    if (out_max == NULL) return ESP_ERR_INVALID_ARG;
    *out_max = 0;
    if (out_capped) *out_capped = false;
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available) { xSemaphoreGive(s_mtx); return ESP_ERR_INVALID_STATE; }
    (void)evlog_flush_writer_locked();
    int64_t lines = 0;
    bool capped = false;
    const TickType_t t0 = xTaskGetTickCount();
    for (uint32_t seq = s_rd_seq; seq <= s_tail_seq && !capped; seq++) {
        evq_rd_t rd;
        uint8_t block;
        esp_err_t oe = evq_open_read_locked(seq, &rd, &block);
        if (oe == ESP_ERR_NOT_FINISHED) { capped = true; break; }
        if (oe != ESP_OK) continue;
        if (seq == s_rd_seq && s_rd_off > 0 && fseek(rd.f, s_rd_off, SEEK_SET) != 0) { evq_rd_close(&rd); continue; }
        while (fgets(s_line, s_line_cap, rd.f) != NULL) {
            size_t len = strlen(s_line);
            if (len == 0 || s_line[len - 1] != '\n') break;   /* partial tail */
            int64_t id = (int64_t)strtoll(s_line, NULL, 10);
            if (id >= from_id && (to_id == 0 || id <= to_id) && id > *out_max) *out_max = id;
            if (++lines >= 3 * EVLOG_SCAN_MAX_LINES ||
                (xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(4 * EVLOG_SCAN_MAX_MS)) {
                capped = true;
                break;
            }
        }
        evq_rd_close(&rd);
    }
    xSemaphoreGive(s_mtx);
    if (out_capped) *out_capped = capped;
    return ESP_OK;
}

esp_err_t event_log_collect_ids_in_range(int64_t from_id, int64_t to_id, int64_t *ids,
                                         size_t cap, size_t *out_n, bool *out_capped)
{
    if (ids == NULL || out_n == NULL) return ESP_ERR_INVALID_ARG;
    *out_n = 0;
    if (out_capped) *out_capped = false;
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available) { xSemaphoreGive(s_mtx); return ESP_ERR_INVALID_STATE; }
    (void)evlog_flush_writer_locked();
    uint32_t min_seq = 0, max_seq = 0;
    bool capped = false;
    const TickType_t t0 = xTaskGetTickCount();
    bool any = evlog_scan_range_locked(&min_seq, &max_seq);
    for (size_t i = 0; i < s_ix.n; i++) {
        if (!any || s_ix.segs[i].seq < min_seq) { min_seq = s_ix.segs[i].seq; any = true; }
    }
    if (any) {
        if (max_seq < s_tail_seq) max_seq = s_tail_seq;
        for (uint32_t seq = min_seq; seq <= max_seq && !capped; seq++) {
            evq_rd_t rd;
            uint8_t block;
            esp_err_t oe = evq_open_read_locked(seq, &rd, &block);
            if (oe == ESP_ERR_NOT_FINISHED) { capped = true; break; }
            if (oe != ESP_OK) continue;
            while (fgets(s_line, s_line_cap, rd.f) != NULL) {
                size_t len = strlen(s_line);
                if (len == 0 || s_line[len - 1] != '\n') break;   /* partial tail */
                int64_t id = (int64_t)strtoll(s_line, NULL, 10);
                if (id >= from_id && (to_id == 0 || id <= to_id)) {
                    if (*out_n >= cap) { capped = true; break; }
                    ids[(*out_n)++] = id;
                }
                if ((xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(4 * EVLOG_SCAN_MAX_MS)) {
                    capped = true;
                    break;
                }
            }
            evq_rd_close(&rd);
        }
    }
    xSemaphoreGive(s_mtx);
    if (out_capped) *out_capped = capped;
    return ESP_OK;
}
