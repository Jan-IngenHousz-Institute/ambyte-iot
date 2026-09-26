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

#ifdef __cplusplus
}
#endif
