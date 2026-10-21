#include "svc_bench.h"

#include <stdio.h>
#include <string.h>

#define BENCH_MAX UINT32_C(1048576)
#define BENCH_EGRESS_MAX 702u

static bool number(const char *s, uint64_t *out)
{ return strlen(s) == 8 && svc_hex(s, 8, out); }
static unsigned char pattern(uint32_t offset)
{ return (unsigned char)(offset * UINT32_C(73) + UINT32_C(19)); }
static void ack(const svc_bench_t *b, uint64_t store, char *out)
{
    /* Identical shape and size to the real durable p ACK; zero is not a
     * durable operation and is never confused with one after bench exit. */
    snprintf(out, SVC_JSON_MAX + 1, "{\"ok\":true,\"store\":\"%016llx\",\"op\":\"0000000000000000\",\"state\":\"staging\",\"offset\":%lu}",
             (unsigned long long)store, (unsigned long)b->offset);
}
static const char *chunk_range(const svc_bench_t *b, uint64_t offset, size_t n)
{
    if (offset > b->offset) return "offset_gap";
    if (offset >= b->size || n > b->size - offset) return "size_limit";
    if (offset % b->chunk || n != (b->size - offset < b->chunk ? b->size - offset : b->chunk))
        return "offset_conflict";
    if (offset < b->offset && offset + n > b->offset) return "offset_conflict";
    return NULL;
}
const char *svc_bench_dispatch(svc_bench_t *b, svc_store_t *store,
                                char *const a[], unsigned n, char *out)
{
    uint64_t size, chunk, offset, crc;
    if (!strcmp(a[0], "bench.begin")) {
        if (n != 4 || (strcmp(a[1], "rx") && strcmp(a[1], "tx")) ||
            !number(a[2], &size) || !number(a[3], &chunk) || !size) return "invalid_args";
        if (size > BENCH_MAX || (chunk != SVC_CHUNK_MAX && (strcmp(a[1], "tx") || chunk != BENCH_EGRESS_MAX)))
            return "size_limit";
        if (b->active) return "busy";
        svc_record_t active;
        const char *error = svc_store_active(store, &active);
        svc_wipe(&active, sizeof active);
        if (!error) return "busy";
        if (strcmp(error, "not_found")) return error;
        *b = (svc_bench_t){.active = true, .transmit = !strcmp(a[1], "tx"),
                             .size = (uint32_t)size, .chunk = (uint16_t)chunk};
        snprintf(out, SVC_JSON_MAX + 1, "{\"ok\":true,\"store\":\"%016llx\",\"state\":\"bench\",\"direction\":\"%s\",\"size\":%lu,\"chunk\":%u}",
                 (unsigned long long)store->instance, b->transmit ? "tx" : "rx", (unsigned long)b->size, b->chunk);
        return NULL;
    }
    if (!strcmp(a[0], "bench.abort")) {
        if (n != 1) return "invalid_args";
        svc_wipe(b, sizeof *b);
        snprintf(out, SVC_JSON_MAX + 1, "{\"ok\":true,\"store\":\"%016llx\",\"state\":\"aborted\"}",
                 (unsigned long long)store->instance);
        return NULL;
    }
    if (!strcmp(a[0], "bench.end")) {
        if (n != 1) return "invalid_args";
        if (!b->active) return "invalid_sequence";
        if (b->offset != b->size) return "incomplete";
        snprintf(out, SVC_JSON_MAX + 1, "{\"ok\":true,\"store\":\"%016llx\",\"state\":\"complete\",\"direction\":\"%s\",\"size\":%lu,\"offset\":%lu,\"chunk\":%u,\"crc32\":\"%08lx\"}",
                 (unsigned long long)store->instance, b->transmit ? "tx" : "rx", (unsigned long)b->size,
                 (unsigned long)b->offset, b->chunk, (unsigned long)b->crc);
        svc_wipe(b, sizeof *b);
        return NULL;
    }
    if (!strcmp(a[0], "p")) {
        if (n != 5 || strcmp(a[1], "0000000000000000") || !number(a[2], &offset) || !number(a[3], &crc))
            return "invalid_args";
        if (!b->active || b->transmit) return "invalid_sequence";
        unsigned char bytes[SVC_CHUNK_MAX + 1] = {0};
        int count = svc_base64_decode(a[4], bytes, sizeof bytes);
        const char *error = count == -2 || count > (int)SVC_CHUNK_MAX ? "size_limit" :
                            count < 1 ? "invalid_args" : NULL;
        if (!error && svc_crc32(0, bytes, (size_t)count) != crc) error = "chunk_crc";
        if (!error) error = chunk_range(b, offset, (size_t)count);
        for (int i = 0; !error && i < count; ++i)
            if (bytes[i] != pattern((uint32_t)offset + (unsigned)i)) error = "checksum_mismatch";
        if (!error) {
            if (offset == b->offset) { b->crc = svc_crc32(b->crc, bytes, (size_t)count); b->offset += (uint32_t)count; }
            ack(b, store->instance, out);
        }
        svc_wipe(bytes, sizeof bytes);
        return error;
    }
    if (!strcmp(a[0], "bench.read")) {
        if (n != 3 || !number(a[1], &offset) || !number(a[2], &chunk) || !chunk) return "invalid_args";
        if (!b->active || !b->transmit) return "invalid_sequence";
        if (chunk > BENCH_EGRESS_MAX) return "size_limit";
        const char *error = chunk_range(b, offset, (size_t)chunk);
        if (error) return error;
        unsigned char bytes[BENCH_EGRESS_MAX];
        for (unsigned i = 0; i < chunk; ++i) bytes[i] = pattern((uint32_t)offset + i);
        crc = svc_crc32(0, bytes, (size_t)chunk);
        int prefix = snprintf(out, SVC_JSON_MAX + 1, "{\"ok\":true,\"store\":\"%016llx\",\"offset\":\"%08lx\",\"crc32\":\"%08lx\",\"data\":\"",
                              (unsigned long long)store->instance, (unsigned long)offset, (unsigned long)crc);
        size_t encoded = 4 * (((size_t)chunk + 2) / 3);
        if (prefix < 0 || (size_t)prefix + encoded + 2 > SVC_JSON_MAX) {
            svc_wipe(bytes, sizeof bytes); return "size_limit";
        }
        svc_base64_encode(bytes, (size_t)chunk, out + prefix);
        memcpy(out + prefix + encoded, "\"}", 3);
        if (offset == b->offset) { b->crc = svc_crc32(b->crc, bytes, (size_t)chunk); b->offset += (uint32_t)chunk; }
        svc_wipe(bytes, sizeof bytes);
        return NULL;
    }
    return "unsupported";
}
