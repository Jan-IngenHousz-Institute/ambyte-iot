#pragma once

/* HIL-only (CONFIG_AMBYTE_EVQ_HIL) relevant-operation trace and fault arming
 * for the on-device verification of the SD overflow (docs/evq-sd-overflow-
 * hil-contract.md). Pure C: no ESP-IDF dependency, so the host test (IO-1)
 * compiles exactly this file.
 *
 * Only RELEVANT operations are traced: SD record-directory ops
 * (/sdcard/events, /sdcard/evq, /sdcard/archive), evq.idx writes/renames and
 * flash ev-*.log remove/rename. Store appends/fsyncs are counters only, so a
 * 3200-record fill cannot wrap the ring with noise. Entries carry a global
 * sequence number; a drain reports total/lost/first/last so the host can prove
 * no relevant entry was lost (contract F-3). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVQ_TR_CAP      8192u
#define EVQ_TR_NAME_MAX 40u

typedef enum {
    EVQ_TR_OPEN_R = 1, EVQ_TR_OPEN_W, EVQ_TR_CLOSE_R, EVQ_TR_CLOSE_W, EVQ_TR_FSYNC,
    EVQ_TR_RENAME, EVQ_TR_REMOVE, EVQ_TR_MKDIR, EVQ_TR_IDX_WRITE, EVQ_TR_FAULT,
} evq_tr_op_t;

typedef struct {
    uint64_t seq;
    int64_t  us;
    uint8_t  op;
    int8_t   result;                  /* 0 ok, else -errno (clamped) */
    uint32_t bytes;                   /* read/written bytes where meaningful */
    char     a[EVQ_TR_NAME_MAX];      /* path tail (dir/basename) */
    char     b[EVQ_TR_NAME_MAX];      /* rename target tail, or idx line prefix */
} evq_tr_entry_t;

typedef struct {
    uint64_t sd_mut, sd_read_open, sd_write_open, flash_mut, store_appends, store_fsyncs;
} evq_io_counters_t;

/* Path classification. */
bool evq_tr_is_sd(const char *path);
bool evq_tr_is_sd_record(const char *path);     /* /sdcard/{events,evq,archive}/... */
bool evq_tr_is_flash_segment(const char *path); /* <evstore>/events/ev-*.log */
bool evq_tr_is_index(const char *path);         /* evq.idx[.tmp] */

/* Ring. `storage` must hold EVQ_TR_CAP entries (PSRAM on target). */
void     evq_tr_init(evq_tr_entry_t *storage);
void     evq_tr_record(int64_t us, evq_tr_op_t op, int result, uint32_t bytes,
                       const char *a, const char *b);
uint64_t evq_tr_total(void);
/* Copy out every entry not yet drained, oldest first, through `emit`. Returns
 * the number of entries emitted and reports how many were overwritten before
 * they could be drained since the previous drain. */
size_t   evq_tr_drain(void (*emit)(const evq_tr_entry_t *e, void *ctx), void *ctx,
                      uint64_t *out_lost, uint64_t *out_first, uint64_t *out_last);

evq_io_counters_t *evq_io_counters(void);

/* Fault arming (one armed point at a time). */
typedef enum { EVQ_ARM_NONE = 0, EVQ_ARM_RESET, EVQ_ARM_RESET_INSIDE, EVQ_ARM_EIO, EVQ_ARM_ENOSPC } evq_arm_mode_t;
void evq_arm_set(const char *point, evq_arm_mode_t mode, unsigned nth);
void evq_arm_clear(void);
/* Called at a named point: returns the action to take now (RESET) or arms
 * the next I/O op (EIO/ENOSPC/RESET_INSIDE) and returns NONE. */
evq_arm_mode_t evq_arm_hit(const char *point);
/* Consumed by the next wrapped I/O op: returns the pending errno (0 = none)
 * or -1 when the op must reset midway. */
int  evq_arm_take_io(void);
bool evq_arm_describe(char *buf, size_t cap);

/* ── writer/op-targeted I/O faults (`evq_hil fault io …`) ────────────────
 * Unlike the named-point arm above ("fail the NEXT op after point P"), these
 * match a specific writer (sd_diag_writer_t, or ANY) and file operation
 * (sd_diag_op_t, or ANY) and count only matching ops, so a test can fail
 * exactly "the 3rd fsync of sd_logger". `count` > 1 keeps firing on the next
 * matches (e.g. to trip the 3-strike SD loss latch deliberately). */
typedef enum {
    EVQ_IOM_NONE = 0,
    EVQ_IOM_EIO,              /* op not performed, -1/EIO */
    EVQ_IOM_ENOSPC,           /* op not performed, -1/ENOSPC */
    EVQ_IOM_SHORT,            /* write only: floor(n/2) bytes written, returned, errno=EIO */
    EVQ_IOM_APPLIED_EIO,      /* op performed, then reported -1/EIO */
    EVQ_IOM_RESET_BEFORE,     /* CPU reset before the op */
    EVQ_IOM_RESET_AFTER,      /* CPU reset after the op completed */
    EVQ_IOM_RESET_MID_WRITE,  /* write only: half written + flushed + synced, then CPU reset */
} evq_iom_t;

#define EVQ_IO_ANY 0xFFu

/* Parse CLI tokens. Returns 0 on success; -1 unknown writer, -2 unknown op,
 * -3 unknown mode, -4 invalid op/mode combination (e.g. short on rename). */
int  evq_arm_io_parse(const char *writer, const char *op, const char *mode,
                      uint8_t *out_w, uint8_t *out_op, evq_iom_t *out_mode);
void evq_arm_io_set(uint8_t writer, uint8_t op, evq_iom_t mode, unsigned nth, unsigned count);
void evq_arm_io_clear(void);
/* Called by a wrapper at every op: returns the action for THIS op. */
evq_iom_t evq_arm_io_hit(uint8_t writer, uint8_t op, unsigned *out_nth);

/* Two targeted slots (Sprint 2 H1). Why two: some recovery branches are only
 * reachable when two DIFFERENT ops fail in one flow (sd_logger rollback after a
 * failed sync, where the truncate must fail too, L-4b); one slot re-armed for
 * the second op would replace the first. Why a path filter: several files share
 * a writer/op (spool primary vs mirror renames), and `nth` alone depends on
 * keeper ordering; the filter makes the target unambiguous and the fired line
 * names the path, so a wrong-path firing is visible, never silently counted.
 * Slot A = 0 (evq_arm_io_set_slot(0, ...) clears BOTH first), slot B = 1
 * (refused, -1, unless A is armed and B is free). An op is offered to A, then B;
 * a slot counts an op only when writer, op and (if set) the path substring all
 * match; at most one slot fires per op (A first). */
int  evq_arm_io_set_slot(int slot, uint8_t writer, uint8_t op, evq_iom_t mode, unsigned nth, unsigned count,
                         const char *path_substr);
evq_iom_t evq_arm_io_hit_p(uint8_t writer, uint8_t op, const char *path, unsigned *out_nth, int *out_slot);
bool evq_arm_io_describe(char *buf, size_t cap);
const char *evq_iom_name(evq_iom_t m);
/* Stable output kind for a fired mode: injected_errno | short_write |
 * applied_then_error | cpu_reset. */
const char *evq_iom_kind(evq_iom_t m);

#ifdef __cplusplus
}
#endif
