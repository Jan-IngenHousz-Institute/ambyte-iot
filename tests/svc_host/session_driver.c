/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "svc_session.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned char input[65536];
static size_t input_length, position, read_size;
static int64_t clock_ms;
static bool held, active, overflow;
static unsigned enters, leaves, writes;
static bool watchdog_model;
/* A scheduling model, not the lock-step bench reproduction: RX/TX are
 * immediately ready, and other tasks are assumed blocked when this task
 * pauses. Crediting a10ms pause is one tick-phase scenario, not a minimum
 * physical duration or a measurement of CPU0's idle task. */
static int64_t last_idle_ms;
static int64_t longest_ready_ms;
static unsigned cooperative_blocks;
static void cpu_work(unsigned ms)
{
    if (!watchdog_model) return;
    clock_ms += ms;
    if(clock_ms-last_idle_ms>longest_ready_ms) longest_ready_ms=clock_ms-last_idle_ms;
    if (clock_ms - last_idle_ms >= 5000) {
        fputs("CPU0 idle starvation: 5000 ms of continuously ready service work\n", stderr);
        abort();
    }
}
int __real_stat(const char *path, struct stat *out);
int __wrap_stat(const char *path, struct stat *out)
{
    int result=__real_stat(path,out);
    cpu_work(100); /* Model synchronous flash CPU work, not an idle wait. */
    return result;
}
static void cooperate(void *context)
{
    (void)context;assert(active && !held);
    clock_ms+=10;last_idle_ms=clock_ms;++cooperative_blocks;
}
static void lock(void *context) { (void)context; assert(!held); held = true; }
static void unlock(void *context) { (void)context; assert(held); held = false; }
static bool ready(void *context) { (void)context; return true; }
static int64_t now(void *context) { (void)context; return clock_ms; }
static bool hash(void *context, const void *bytes, size_t n, unsigned char out[32])
{
    (void)context; unsigned size;
    return EVP_Digest(bytes, n, out, &size, EVP_sha256(), NULL) == 1 && size == 32;
}
static size_t free_bytes(void *context) { (void)context; return 1024 * 1024; }
void svc_store_fault(const char *point) { (void)point; }
static bool enter(void *context)
{
    (void)context; assert(!active && !held); active = true; ++enters;
    watchdog_model=getenv("SVC_WDT_MODEL")!=NULL;last_idle_ms=clock_ms;
    return !getenv("SVC_ENTER_FAIL");
}
static void leave(void *context)
{
    (void)context; assert(active && !held); active = false; ++leaves;
}
static bool write_frame(void *context, const void *bytes, size_t n)
{
    (void)context; assert(active && !held && n <= SVC_FRAME_MAX); ++writes;
    cpu_work(10); /* A writable TX ring need not block the console task. */
    if (getenv("SVC_TX_FAIL") && writes == strtoul(getenv("SVC_TX_FAIL"), NULL, 10)) {
        assert(fwrite(bytes, 1, 5, stdout) == 5); return false;
    }
    assert(fwrite(bytes, 1, n, stdout) == n); return true;
}
static bool overrun(void *context) { (void)context; bool out = overflow; overflow = false; return out; }
static int read_bytes(void *context, void *bytes, size_t capacity, unsigned wait)
{
    (void)context; assert(active && !held && capacity == 64);
    for (size_t i = 0; i < capacity; ++i) assert(((unsigned char *)bytes)[i] == 0);
    if (position == input_length) { clock_ms += wait; if (wait) last_idle_ms=clock_ms; return 0; }
    if (input[position] == 0x1e) { ++position; clock_ms += 2100; return 0; }
    if (input[position] == 0x1f) { ++position; overflow = true; }
    size_t n = 0;
    while (n < capacity && n < read_size && position < input_length && input[position] != 0x1e && input[position] != 0x1f)
        ((unsigned char *)bytes)[n++] = input[position++];
    cpu_work(1);++clock_ms;
    return (int)n;
}
int main(int argc, char **argv)
{
    assert(argc == 3); read_size = strtoul(argv[2], NULL, 10);
    input_length = fread(input, 1, sizeof input, stdin); assert(!ferror(stdin));
    svc_store_t store;
    assert(!svc_store_open(&store, argv[1], UINT64_C(0x0123456789abcdef), free_bytes, NULL));
    svc_engine_ports_t engine_ports = {.protocol = {.sha256 = hash}, .lock = lock, .unlock = unlock,
                                       .boot_ready = ready, .now_ms = now};
    svc_engine_t engine; svc_engine_init(&engine, &store, &engine_ports);
    if (getenv("SVC_STALE_SESSION")) engine.protocol.expected_instance = UINT64_C(0x2222222222222222);
    svc_session_ports_t ports = {.enter = enter, .leave = leave, .read = read_bytes, .write = write_frame,
                                  .overrun = overrun, .now_ms = now, .cooperate = cooperate};
    (void)svc_session_run(&engine, &ports);
    assert(!active && !held && enters == 1 && leaves == 1 && !engine.protocol.bound_instance);
    assert(!engine.protocol.expected_instance);
    if (watchdog_model) {
        assert(cooperative_blocks>100);
        fprintf(stderr,"WDT_FAIRNESS_OK blocks=%u max_ready_ms=%lld\n",cooperative_blocks,(long long)longest_ready_ms);
    }
    svc_wipe(input, sizeof input);
}
