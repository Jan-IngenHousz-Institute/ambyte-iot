/*
 * prelude.h — force-included (-include) into every production translation unit
 * and the harness TU of the evq integration harness. It is the ONLY host seam:
 *
 *   - wall clock: time()/gettimeofday() read the shim's VIRTUAL clock, so the
 *     publish-power debounce, the clock gate and envelope timestamps follow the
 *     same simulated time as the FreeRTOS tick and esp_timer_get_time();
 *   - media shim: fopen/fsync/rename/remove/stat/opendir/mkdir are routed
 *     through thin wrappers (esp_stubs.c) that forward to libc unchanged, but
 *     (a) enforce the configured internal-flash capacity on ./evstore (ENOSPC on
 *     create/fsync once the block-rounded usage exceeds it, like a full littlefs
 *     partition), (b) allow a one-shot EIO on the next flash fsync, and
 *     (c) count/trace SD-path operations per task (bracket discipline checks).
 *
 * The system headers whose prototypes the macros would rename are included
 * first, so their include guards keep the real declarations intact.
 * esp_stubs.c / rtos_shim.c (which implement the wrappers) are compiled with
 * -DEVQ_INTEG_IMPL_TU and see none of this.
 */
#pragma once
#ifndef EVQ_INTEG_IMPL_TU

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

time_t h_time(time_t *out);
int    h_gettimeofday(struct timeval *tv, void *tz);
FILE  *h_fopen(const char *path, const char *mode);
int    h_fsync(int fd);
int    h_rename(const char *from, const char *to);
int    h_remove(const char *path);
int    h_stat(const char *path, struct stat *st);
DIR   *h_opendir(const char *path);
int    h_mkdir(const char *path, mode_t mode);

#define time(t)            h_time(t)
#define gettimeofday(tv, z) h_gettimeofday((tv), (z))
#define fopen(p, m)        h_fopen((p), (m))
#define fsync(fd)          h_fsync(fd)
#define rename(a, b)       h_rename((a), (b))
#define remove(p)          h_remove(p)
#define stat(p, b)         h_stat((p), (b))
#define opendir(p)         h_opendir(p)
#define mkdir(p, m)        h_mkdir((p), (m))

#endif
