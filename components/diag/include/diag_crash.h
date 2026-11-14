#pragma once
/*
 * diag_crash.h — summary of the stored coredump, as carried by every
 * heartbeat in health.crash (remote diagnostics, 2026-11).
 *
 * The coredump partition (0x610000, 64 KiB, ELF + CRC32) already captures
 * every panic, but nothing ever read it back: the evidence never left the
 * device. diag_boot.c reads it ONCE per boot in app_main's task context (never
 * an ISR, never the heartbeat path), fills this struct, and renders it into a
 * cached JSON object; the heartbeat only copies that string.
 *
 * Read-only by design: the dump is NEVER erased by this feature. A dump
 * therefore keeps being reported on every boot until something else replaces
 * it (the next panic overwrites it). `first_seen_boot` is what makes a stale
 * dump from weeks ago recognisable as old: it is the boot_count on which this
 * exact dump (identity = size + checksum tail, see diag_boot.c) was first
 * seen, persisted in NVS.
 *
 * This header and diag_crash.c are pure C (no IDF): tests/diag_host renders
 * present / absent / corrupt summaries against them.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_CRASH_BT_MAX     16U   /* IDF's esp_core_dump_bt_info_t.bt[16] */
#define DIAG_CRASH_SHA_HEX    16U   /* app ELF SHA-256 prefix carried on the wire */
/* Worst case: 16 backtrace strings at 13 B, a fully \u-escaped 15-char task
 * name (90 B), a ~40 B error name and the fixed keys (~260 B) — ≈ 600 B. */
#define DIAG_CRASH_JSON_CAP   704U

typedef struct {
    bool present;              /* the partition holds a (possibly corrupt) dump */
    /* Non-NULL when the dump (or the partition) could not be read/verified:
     * an esp_err_to_name() string or a short fixed reason. Rendered as
     * "error"; the summary fields below are then omitted. A corrupt dump is
     * still present:true — it is evidence that SOMETHING crashed. */
    const char *error;
    bool summary_valid;        /* the fields below came from esp_core_dump_get_summary */
    char app_elf_sha256[DIAG_CRASH_SHA_HEX + 1];
    char task[17];             /* FreeRTOS task name (configMAX_TASK_NAME_LEN 16) */
    uint32_t pc;
    uint32_t exc_cause;        /* Xtensa EXCCAUSE */
    uint32_t exc_vaddr;        /* Xtensa EXCVADDR (faulting data address) */
    uint32_t backtrace[DIAG_CRASH_BT_MAX];
    uint32_t bt_depth;         /* clamped to DIAG_CRASH_BT_MAX by the renderer */
    bool bt_corrupted;
    /* Stable identity of the dump image; 0 = could not be derived. */
    uint64_t id;
    bool first_seen_valid;
    uint32_t first_seen_boot;  /* boot_count on which this id was first seen */
} diag_crash_t;

/* Render `c` as one JSON object into out. Returns the length, or -1 when it
 * does not fit (never a truncated object). Absent → {"present":false}. */
int diag_crash_render_json(const diag_crash_t *c, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
