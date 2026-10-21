#pragma once
/* Recursive newlib stream locks: the harness schedules callers serially. */
typedef int _lock_t;
static inline void _lock_acquire_recursive(_lock_t *p) { ++*p; }
static inline void _lock_release_recursive(_lock_t *p) { --*p; }

#include <assert.h>
