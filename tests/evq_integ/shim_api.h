/* Harness-side control surface of the shim + stubs (never included by production code). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── scheduler / virtual clock (rtos_shim.c) ── */
typedef struct {
    uint64_t switches, time_advances;
    uint64_t sem_takes, sem_contended, sem_timeouts, queue_full;
    uint64_t take_while_sd_ref;      /* mutex acquisitions while the taker holds an SD ref */
    char     traced[8][24];
    int      ntraced;
} h_shim_stats_t;
extern h_shim_stats_t h_shim_stats;

void        h_shim_trace_task(const char *name);   /* call before the task is created */
void        h_shim_start(const char *name, int prio);
uint64_t    h_vt_us(void);
void        h_set_epoch_base(int64_t epoch_s);
const char *h_task_name(void);

typedef struct {
    uint64_t vt_us;
    char     task[16];
    char     kind[20];
    char     a[48];
    int64_t  n;
} h_trace_t;
void             h_trace(const char *kind, const char *a, int64_t n);
void             h_trace_locked_ext(uint64_t vt, const char *task, const char *kind, const char *a, int64_t n);
size_t           h_trace_count(void);
const h_trace_t *h_trace_at(size_t i);

typedef void (*h_log_hook_fn)(char level, const char *tag, const char *msg);
void h_log_open(const char *path);
void h_log_set_hook(h_log_hook_fn fn);

/* ── media + stubs (esp_stubs.c) ── */
typedef struct {
    size_t   flash_total;            /* esp_littlefs_info total */
    uint64_t sd_total;               /* sdcard_free_bytes = sd_total - used(./sdcard) */
    bool     sd_mounted;
    uint32_t sd_cid;
    bool     sd_io_lost;
    bool     flash_eio_next_fsync;   /* one-shot EIO on the next ./evstore fsync */
    size_t   heap_internal_largest;  /* heap_caps_get_largest_free_block(INTERNAL|DMA) */
    char     app_version[32];
} h_media_cfg_t;
extern h_media_cfg_t h_media;

typedef struct {
    uint64_t io_begin, io_begin_refused, io_end;
    uint64_t report_err, report_ok;
    int      refs_now, refs_max;
    uint64_t sd_ops;                 /* fopen/rename/remove/stat/opendir/mkdir on ./sdcard */
    uint64_t sd_ops_outside_bracket; /* ... by a task holding no SD ref (test task excluded) */
    uint64_t flash_enospc, flash_eio;
    uint64_t unmount_with_refs;
} h_media_stats_t;
extern h_media_stats_t h_mstats;

int      h_sd_refs_held(void);              /* SD refs held by the calling thread */
uint64_t h_dir_used_bytes(const char *dir); /* block-rounded usage */
void     h_nvs_set_path(const char *path);  /* persisted NVS image (survives a "reboot") */
bool     h_restart_allowed_set(bool allowed);
