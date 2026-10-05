#pragma once

/* Writer-tagged SD fault wrappers for the verification build only
 * (CONFIG_AMBYTE_EVQ_HIL, env evq-hil; `evq_hil fault io <writer> <op> <mode>`).
 *
 * A writer translation unit defines EVQ_HIL_WRITER (an sd_diag_writer_t) and
 * includes this header LAST, after every system header, so only that file's own
 * file operations are rerouted. In any other build — the release image and the
 * host harnesses — this header defines nothing at all: the release never
 * carries a fault hook, and the host tests use their own media shims.
 *
 * Each wrapper passes straight through to libc unless the armed fault matches
 * (writer, op, nth): then it injects EIO/ENOSPC, a short write, an applied-
 * then-failed op, or a CPU reset (esp_rom_software_reset_system — the SD card's
 * power is NOT interrupted; see evq_hil_io.c). */

#if !defined(EVQ_HOST_FAULTS) && !defined(EVQ_HIL_HOST) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#if defined(CONFIG_AMBYTE_EVQ_HIL) && CONFIG_AMBYTE_EVQ_HIL && !defined(EVQ_HOST_FAULTS) && !defined(EVQ_HIL_HOST)

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef EVQ_HIL_WRITER
#error "define EVQ_HIL_WRITER (an sd_diag_writer_t) before including evq_hil_io_w.h"
#endif

FILE  *evq_hil_io_fopen_w(uint8_t w, const char *path, const char *mode);
int    evq_hil_io_fclose_w(uint8_t w, FILE *f);
size_t evq_hil_io_fwrite_w(uint8_t w, const void *p, size_t sz, size_t n, FILE *f);
size_t evq_hil_io_fread_w(uint8_t w, void *p, size_t sz, size_t n, FILE *f);
int    evq_hil_io_fflush_w(uint8_t w, FILE *f);
int    evq_hil_io_fsync_w(uint8_t w, int fd);
int    evq_hil_io_ftruncate_w(uint8_t w, int fd, off_t len);
int    evq_hil_io_rename_w(uint8_t w, const char *a, const char *b);
int    evq_hil_io_remove_w(uint8_t w, const char *p);
int    evq_hil_io_mkdir_w(uint8_t w, const char *p, mode_t m);
int    evq_hil_io_stat_w(uint8_t w, const char *p, struct stat *st);
DIR   *evq_hil_io_opendir_w(uint8_t w, const char *p);

#define fopen(p, m)          evq_hil_io_fopen_w(EVQ_HIL_WRITER, p, m)
#define fclose(f)            evq_hil_io_fclose_w(EVQ_HIL_WRITER, f)
#define fwrite(p, s, n, f)   evq_hil_io_fwrite_w(EVQ_HIL_WRITER, p, s, n, f)
#define fread(p, s, n, f)    evq_hil_io_fread_w(EVQ_HIL_WRITER, p, s, n, f)
#define fflush(f)            evq_hil_io_fflush_w(EVQ_HIL_WRITER, f)
#define fsync(fd)            evq_hil_io_fsync_w(EVQ_HIL_WRITER, fd)
#define ftruncate(fd, l)     evq_hil_io_ftruncate_w(EVQ_HIL_WRITER, fd, l)
#define rename(a, b)         evq_hil_io_rename_w(EVQ_HIL_WRITER, a, b)
#define remove(p)            evq_hil_io_remove_w(EVQ_HIL_WRITER, p)
#define mkdir(p, m)          evq_hil_io_mkdir_w(EVQ_HIL_WRITER, p, m)
#define stat(p, st)          evq_hil_io_stat_w(EVQ_HIL_WRITER, p, st)
#define opendir(p)           evq_hil_io_opendir_w(EVQ_HIL_WRITER, p)

#endif
