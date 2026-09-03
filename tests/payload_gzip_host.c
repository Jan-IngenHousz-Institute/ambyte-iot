/* Host harness for the production transport-gzip helper (payload_gzip.c).
 *
 * The device deflates with the ESP32-S3 ROM tdefl compressor, which cannot run
 * on the host; everything this module OWNS — RFC 1952 framing, CRC-32, ISIZE,
 * base64 — is deflate-agnostic, so the harness injects a dependency-free
 * stored-block (BTYPE=00) DEFLATE provider. Python's gzip module (the exact
 * codec the OpenJII Silver decompress_sample_value uses) then round-trips the
 * emitted streams in test_payload_gzip.py. Records print as KEY=VALUE lines;
 * internal invariants fail the process. */

#include "payload_gzip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void fail(const char *what)
{
    fprintf(stderr, "FAILED: %s\n", what);
    exit(1);
}

static void require(bool ok, const char *what)
{
    if (!ok) fail(what);
}

/* RFC 1951 stored blocks: 5-byte header per <=65,535-byte chunk. Emits one
 * final empty block for empty input, which keeps zero-length payloads valid. */
static bool stored_deflate(void *ctx, const uint8_t *src, size_t src_len,
                           uint8_t *out, size_t out_cap, size_t *out_len)
{
    (void)ctx;
    size_t o = 0;
    size_t i = 0;
    do {
        size_t chunk = src_len - i > 65535U ? 65535U : src_len - i;
        bool final = i + chunk == src_len;
        if (o + 5U + chunk > out_cap) return false;
        out[o++] = final ? 1U : 0U;
        out[o++] = (uint8_t)(chunk & 0xFFU);
        out[o++] = (uint8_t)(chunk >> 8);
        out[o++] = (uint8_t)(~chunk & 0xFFU);
        out[o++] = (uint8_t)((~chunk >> 8) & 0xFFU);
        memcpy(out + o, src + i, chunk);
        o += chunk;
        i += chunk;
    } while (i < src_len);
    *out_len = o;
    return true;
}

static bool failing_deflate(void *ctx, const uint8_t *src, size_t src_len,
                            uint8_t *out, size_t out_cap, size_t *out_len)
{
    (void)ctx; (void)src; (void)src_len; (void)out; (void)out_cap; (void)out_len;
    return false;
}

/* The same compact ambit.trace/3 shape the device wraps as "[<object>]". */
static const char FIXTURE[] =
    "[{\"schema\":\"ambit.trace/3\",\"measure_id\":26337,"
    "\"channel\":\"uart_1\",\"device\":\"AmbitV003\",\"tag\":\"MEASUREMENT\","
    "\"time\":{\"start_utc\":1785965160359,\"end_utc\":1785965213985},"
    "\"protocol\":{\"name\":\"SS\",\"cmd\":\"arrun 1,0,2,0,0,59,0,1,0,1\","
    "\"segments\":[{\"pulses\":59,\"freq\":1,\"actinic\":0}],"
    "\"tick_factor\":0.854,\"gains\":[1,1,2,2,1,1]},"
    "\"series\":{\"fluo_630_signal\":{\"u\":\"count\",\"t0\":0,\"dt\":0.854,"
    "\"v\":[159,164,171,166,158,162,170,169,161,157]}}}]";

static void emit_b64(const char *key, const uint8_t *src, size_t src_len,
                     payload_gzip_deflate_fn deflate)
{
    size_t scratch_cap = payload_gzip_stream_cap(payload_gzip_deflate_bound(src_len));
    uint8_t *scratch = malloc(scratch_cap);
    size_t b64_cap = payload_gzip_base64_cap(scratch_cap);
    char *b64 = malloc(b64_cap);
    if (scratch == NULL || b64 == NULL) fail("harness alloc");
    size_t b64_len = 0;
    require(payload_gzip_encode_base64(src, src_len, deflate, NULL,
                                       scratch, scratch_cap,
                                       b64, b64_cap, &b64_len),
            "encode_base64 succeeds");
    require(b64_len == strlen(b64), "reported base64 length matches text");
    printf("%s=%s\n", key, b64);
    free(scratch);
    free(b64);
}

int main(void)
{
    /* CRC-32 check vector (IEEE/gzip): crc32("123456789") == 0xCBF43926. */
    require(payload_gzip_crc32(0, "123456789", 9) == 0xCBF43926U, "crc32 vector");
    /* Incremental chaining equals one-shot. */
    uint32_t part = payload_gzip_crc32(0, "12345", 5);
    require(payload_gzip_crc32(part, "6789", 4) == 0xCBF43926U, "crc32 chaining");

    /* RFC 4648 base64 vectors. */
    char b64[16];
    size_t b64_len = 0;
    require(payload_gzip_base64_encode((const uint8_t *)"", 0, b64, sizeof b64, &b64_len)
                && b64_len == 0 && strcmp(b64, "") == 0, "b64 empty");
    require(payload_gzip_base64_encode((const uint8_t *)"f", 1, b64, sizeof b64, &b64_len)
                && strcmp(b64, "Zg==") == 0, "b64 f");
    require(payload_gzip_base64_encode((const uint8_t *)"fo", 2, b64, sizeof b64, &b64_len)
                && strcmp(b64, "Zm8=") == 0, "b64 fo");
    require(payload_gzip_base64_encode((const uint8_t *)"foob", 4, b64, sizeof b64, &b64_len)
                && strcmp(b64, "Zm9vYg==") == 0, "b64 foob");
    require(!payload_gzip_base64_encode((const uint8_t *)"foob", 4, b64, 8, NULL),
            "b64 cap too small fails");

    /* Failure paths stay closed. */
    uint8_t scratch[64];
    char out[64];
    require(!payload_gzip_encode_base64((const uint8_t *)"x", 1,
                                        failing_deflate, NULL,
                                        scratch, sizeof scratch,
                                        out, sizeof out, NULL),
            "deflate failure propagates");
    require(!payload_gzip_encode_base64((const uint8_t *)"x", 1,
                                        stored_deflate, NULL,
                                        scratch, 17, /* < header+trailer+5+1 */
                                        out, sizeof out, NULL),
            "scratch too small fails");

    /* Round-trip records for Python gzip validation. */
    emit_b64("GZ_FIXTURE", (const uint8_t *)FIXTURE, strlen(FIXTURE), stored_deflate);
    emit_b64("GZ_EMPTY", (const uint8_t *)"", 0, stored_deflate);

    /* Multi-block: force several stored blocks so CRC/ISIZE cover >64 KiB. */
    size_t big_len = 200000U;
    uint8_t *big = malloc(big_len);
    if (big == NULL) fail("big alloc");
    for (size_t i = 0; i < big_len; ++i) big[i] = (uint8_t)('0' + i % 75U);
    emit_b64("GZ_BIG", big, big_len, stored_deflate);

    /* Cross-check payload sources the Python side re-derives. */
    printf("SRC_FIXTURE=%s\n", FIXTURE);
    printf("SRC_BIG_LEN=%zu\n", big_len);
    free(big);
    return 0;
}
