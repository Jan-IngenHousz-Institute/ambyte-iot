#pragma once

/* HIL-only I/O wrapper for event_log.c (CONFIG_AMBYTE_EVQ_HIL). Included by
 * event_log.c AFTER all of its system/IDF headers, so only event_log's own
 * file operations are rerouted — the on-device twin of tests/evq_host/fsshim.
 * Each wrapper passes straight through to libc, counts the op, records
 * relevant ops in the evq_hil_trace ring and consumes an armed fault
 * (EIO/ENOSPC on this op, or a ROM reset midway through a write). */

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

FILE  *evq_hil_fopen(const char *path, const char *mode);
int    evq_hil_fclose(FILE *f);
size_t evq_hil_fwrite(const void *p, size_t sz, size_t n, FILE *f);
size_t evq_hil_fread(void *p, size_t sz, size_t n, FILE *f);
int    evq_hil_fflush(FILE *f);
int    evq_hil_fsync(int fd);
int    evq_hil_rename(const char *a, const char *b);
int    evq_hil_remove(const char *p);
int    evq_hil_mkdir(const char *p, mode_t m);
int    evq_hil_stat(const char *p, struct stat *st);
DIR   *evq_hil_opendir(const char *p);
int    evq_hil_ferror(FILE *f);
void   evq_hil_clearerr(FILE *f);

#define fopen(p, m)          evq_hil_fopen(p, m)
#define fclose(f)            evq_hil_fclose(f)
#define fwrite(p, s, n, f)   evq_hil_fwrite(p, s, n, f)
#define fread(p, s, n, f)    evq_hil_fread(p, s, n, f)
#define fflush(f)            evq_hil_fflush(f)
#define fsync(fd)            evq_hil_fsync(fd)
#define rename(a, b)         evq_hil_rename(a, b)
#define remove(p)            evq_hil_remove(p)
#define mkdir(p, m)          evq_hil_mkdir(p, m)
#define stat(p, st)          evq_hil_stat(p, st)
#define opendir(p)           evq_hil_opendir(p)
#define ferror(f)            evq_hil_ferror(f)
#define clearerr(f)          evq_hil_clearerr(f)
