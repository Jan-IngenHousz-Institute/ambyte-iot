#pragma once

/* Named persistence-boundary hooks for the host fault harness
 * (tests/evq_host). On target they compile to nothing: EVQ_HOST_FAULTS is only
 * ever defined by the host build, and J3/check_constants.py asserts the
 * production build never sees it.
 *
 * Semantics (host): reaching a point whose name is armed via EVQ_FAULT either
 * crashes the process right there (simulated power loss), or — for eio/enospc
 * modes and for "*.inside*" names — arms the media shim so the NEXT file
 * operation fails or crashes midway. Each point is also counted, so the matrix
 * can prove every registered point was reached. */
#ifdef EVQ_HOST_FAULTS
void evq_fault_point(const char *name);
#define EVQ_FAULT_POINT(name) evq_fault_point(name)
#else
#define EVQ_FAULT_POINT(name) ((void)0)
#endif
