/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "svc_session.h"
#include <assert.h>
#include <openssl/evp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool held, active;
static svc_store_t store;
static svc_engine_t engine;
static void (*pending)(void *);
static void *pending_arg;
static uint64_t applied_id;
static unsigned applies;
static bool initialized;
static unsigned boot_checks_remaining;
static void lock(void *context) { (void)context; assert(!held); held = true; }
static void unlock(void *context) { (void)context; assert(held); held = false; }
static bool ready(void *context)
{
    (void)context;
    if (!boot_checks_remaining) return true;
    svc_record_t record;
    if (!svc_store_active(&store, &record) && record.kind == SVC_SCHEDULE && record.offset == record.size)
        --boot_checks_remaining;
    return false;
}
static int64_t now(void *context)
{
    (void)context; struct timespec time; assert(!clock_gettime(CLOCK_MONOTONIC, &time));
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}
static bool hash(void *context, const void *bytes, size_t n, unsigned char out[32])
{
    (void)context; unsigned size;
    return EVP_Digest(bytes, n, out, &size, EVP_sha256(), NULL) == 1 && size == 32;
}
static size_t free_bytes(void *context) { (void)context; return 1024 * 1024; }
void svc_store_fault(const char *point)
{
    (void)point;
    /* The benchmark test allows initialization, then makes every persistent
     * mutation boundary fatal. A no-op rewrite is caught as well as new data. */
    assert(!initialized || !getenv("SVC_ASSERT_NO_WRITES"));
}
static bool submit(void *context, void (*run)(void *), void *arg)
{ (void)context; assert(held && !pending); pending = run; pending_arg = arg; return true; }
static bool maintenance_begin(void *context) { (void)context; return true; }
static void maintenance_end(void *context) { (void)context; }
/* This is the installer boundary, not an alternate service implementation.
 * The real private installer/compiler/provider has its separate whole-TU
 * harness. Here exact bytes are hashed independently before activation. */
static const char *apply(void *context, const svc_record_t *r, svc_truth_t *changed)
{
    (void)context; char path[192]; assert(svc_store_data_path(&store, path, sizeof path));
    FILE *f = fopen(path, "rb"); assert(f);
    unsigned char *bytes = malloc(SVC_DATA_MAX + 1); assert(bytes);
    size_t n = fread(bytes, 1, SVC_DATA_MAX + 1, f); assert(!ferror(f) && !fclose(f));
    unsigned char digest[32]; char hex[65]; assert(hash(NULL, bytes, n, digest));
    for (unsigned i = 0; i < 32; ++i) sprintf(hex + 2 * i, "%02x", digest[i]);
    free(bytes);
    if (n != r->size || strcmp(hex, r->sha256)) return "checksum_mismatch";
    applied_id = r->id; ++applies; *changed = SVC_TRUE; return NULL;
}
static bool matches(void *context, const svc_record_t *r) { (void)context; return applied_id == r->id; }
static bool enter(void *context) { (void)context; assert(!active); active = true; return true; }
static void leave(void *context) { (void)context; assert(active); active = false; }
static bool write_bytes(void *context, const void *bytes, size_t n)
{ (void)context; return fwrite(bytes, 1, n, stdout) == n && !fflush(stdout); }
static bool overrun(void *context) { (void)context; return false; }
static int read_bytes(void *context, void *bytes, size_t capacity, unsigned wait)
{
    (void)context; assert(active && !held);
    if (pending) { void (*run)(void *) = pending; pending = NULL; run(pending_arg); }
    struct pollfd fd = {.fd = STDIN_FILENO, .events = POLLIN};
    int rc = poll(&fd, 1, (int)wait);
    if (rc < 0) return -1;
    if (!rc) return 0;
    ssize_t n = read(STDIN_FILENO, bytes, capacity);
    return n > 0 ? (int)n : -1;
}
int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    if (argc == 3) boot_checks_remaining = (unsigned)strtoul(argv[2], NULL, 10);
    assert(!svc_store_open(&store, argv[1], UINT64_C(0x0123456789abcdef), free_bytes, NULL));
    svc_engine_ports_t e = {.protocol = {.sha256 = hash}, .lock = lock, .unlock = unlock,
        .boot_ready = ready, .now_ms = now, .submit = submit,
        .maintenance_begin = maintenance_begin, .maintenance_end = maintenance_end,
        .schedule_apply = apply, .schedule_matches = matches};
    svc_engine_init(&engine, &store, &e);
    initialized = true;
    svc_session_ports_t io = {.enter = enter, .leave = leave, .read = read_bytes,
        .write = write_bytes, .overrun = overrun, .now_ms = now};
    char line[249]; size_t length = 0; unsigned char byte;
    while (read(STDIN_FILENO, &byte, 1) == 1) {
        assert(length < sizeof line - 1);
        line[length++] = (char)byte;
        if (byte != '\n') continue;
        line[length] = 0;
        if (!strcmp(line, "svc\n")) (void)svc_session_run(&engine, &io);
        else {
            char json[SVC_JSON_MAX + 1], frame[SVC_FRAME_MAX + 1];
            svc_protocol_error(&engine.protocol, "mode_required", json);
            size_t n = svc_frame(frame, sizeof frame, 0, json); assert(n);
            assert(write_bytes(NULL, frame, n));
        }
        svc_wipe(line, sizeof line); length = 0;
    }
    fprintf(stderr, "apply_count=%u\n", applies);
    return 0;
}
