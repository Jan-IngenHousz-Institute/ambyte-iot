#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SD_CARD_PATH_MAX
#define SD_CARD_PATH_MAX 260
#endif

#ifndef SD_MOUNT_POINT
#define SD_MOUNT_POINT "/sdcard"
#endif

// Initializes the SD service with the board-default SDMMC wiring.
esp_err_t sdcard_init_default(void);

// Mounts the SD card once and keeps it mounted until sdcard_unmount() is called.
esp_err_t sdcard_mount(void);
esp_err_t sdcard_unmount(void);
bool sdcard_is_mounted(void);

// Hot-plug monitoring (no card-detect pin → software polling).
//
// sdcard_start_monitor() spawns a low-priority task that wakes every period_ms,
// probes the card with sdmmc_get_status() when mounted (CMD13 — fails on a
// pulled card) and attempts a remount when unmounted. On every mount-state
// transition it calls `cb(mounted)`, so callers (persistence, Lua) can react.
// Safe to call once after sdcard_mount(); subsequent calls are no-ops.
//
// The CMD13 poll alone is not enough: it serializes through the same SDMMC host
// mutex as the failing reads/writes and loses the race for the sd_card lock to a
// task stuck in a multi-second failing transfer, so a pulled card can go
// undetected indefinitely. The error-driven path below closes that gap.
typedef void (*sdcard_state_cb_t)(bool mounted);
esp_err_t sdcard_start_monitor(uint32_t period_ms, sdcard_state_cb_t cb);

// Error-driven card-loss signalling (lock-free; safe from any task).
//
// Any task doing SD file I/O calls sdcard_report_io_error() when an op fails and
// sdcard_report_io_ok() when one succeeds. After a short run of consecutive
// failures (a pulled/dead card), the loss is latched and the hot-plug monitor is
// woken to tear the mount down immediately — without waiting on the CMD13 poll or
// the contended sd_card lock. sdcard_io_lost() is a cheap, lock-free gate writers
// check BEFORE attempting I/O so they stop re-arming failing ops at once. The
// latch clears only on a successful remount.
void sdcard_report_io_error(void);
void sdcard_report_io_ok(void);
bool sdcard_io_lost(void);

#ifdef __cplusplus
}
#endif
