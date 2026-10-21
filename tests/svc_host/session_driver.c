/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "svc_session.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char input[65536];
static size_t input_length, position, read_size;
static int64_t clock_ms;
static bool held, active, overflow;
static unsigned enters, leaves, writes;
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
    return !getenv("SVC_ENTER_FAIL");
}
static void leave(void *context)
{
    (void)context; assert(active && !held); active = false; ++leaves;
}
static bool write_frame(void *context, const void *bytes, size_t n)
{
    (void)context; assert(active && !held && n <= SVC_FRAME_MAX); ++writes;
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
    if (position == input_length) { clock_ms += wait; return 0; }
    if (input[position] == 0x1e) { ++position; clock_ms += 2100; return 0; }
    if (input[position] == 0x1f) { ++position; overflow = true; }
    size_t n = 0;
    while (n < capacity && n < read_size && position < input_length && input[position] != 0x1e && input[position] != 0x1f)
        ((unsigned char *)bytes)[n++] = input[position++];
    ++clock_ms;
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
    svc_session_ports_t ports = {.enter = enter, .leave = leave, .read = read_bytes, .write = write_frame,
                                  .overrun = overrun, .now_ms = now};
    (void)svc_session_run(&engine, &ports);
    assert(!active && !held && enters == 1 && leaves == 1 && !engine.protocol.bound_instance);
    svc_wipe(input, sizeof input);
}
