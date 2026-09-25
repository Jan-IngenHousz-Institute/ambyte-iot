/* Shared declarations for the storage harness stubs, shim and driver. */
#ifndef EVQ_HOST_H
#define EVQ_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* sd_stub.c */
bool     evq_sd_stub_mounted(void);
void     evq_sd_stub_set(bool mounted, uint32_t cid);
uint32_t evq_sd_stub_cid(void);
int      evq_sd_stub_refs(void);
void     evq_sd_stub_set_lost(bool lost);

/* rtos_stub.c */
void     evq_clock_advance(uint32_t ms);
uint32_t evq_clock_now(void);

/* fsshim.c */
void      shim_seed(uint64_t seed);
void      shim_arm_errno(int e);
void      shim_arm_crash_inside(void);
void      shim_rescan(void);
void      shim_mark(const char *what);
uint64_t  shim_sd_ops(void);
uint64_t  shim_flash_ops(void);
long long shim_used(int medium);
long long shim_cap(int medium);
int       shim_flash_info(size_t *total, size_t *used);
long long shim_sd_free(void);

/* driver (evq_driver.c): flush manifests before a simulated power loss */
void evq_host_flush_manifests(void);

#endif
