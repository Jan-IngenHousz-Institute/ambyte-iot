#ifndef AMBYTE_PAYLOAD_GZIP_H
#define AMBYTE_PAYLOAD_GZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Transport gzip helper for the MQTT publish path.
 *
 * The OpenJII pipeline reverses exactly base64(gzip(<sample JSON text>)) when
 * the envelope carries `_sample_encoding: "gzip+base64"` (Silver
 * decompress_sample_value). This module owns everything that must be
 * byte-identical between host regressions and the device — the RFC 1952 gzip
 * framing (header, CRC-32, ISIZE) and the base64 text — while the raw-DEFLATE
 * body is injected by the platform: the ESP32-S3 ROM tdefl compressor in
 * production, a dependency-free stored-block shim in host tests. Like
 * payload_v3, nothing here allocates; callers own every buffer.
 */

/* Produces one complete raw DEFLATE stream (RFC 1951, no zlib/gzip wrapper)
 * for src into out. Returns false when out_cap cannot hold the stream or the
 * compressor fails; *out_len receives the stream length on success. */
typedef bool (*payload_gzip_deflate_fn)(void *ctx,
                                        const uint8_t *src, size_t src_len,
                                        uint8_t *out, size_t out_cap,
                                        size_t *out_len);

/* Worst-case raw-DEFLATE size for incompressible input: stored blocks carry a
 * 5-byte header per 65,535-byte chunk plus the final-block/EOB slack. Platform
 * deflate providers must stay within this bound so one sizing rule covers the
 * ROM compressor and the host shim. */
size_t payload_gzip_deflate_bound(size_t src_len);

/* Full gzip stream cap for a deflate body of at most deflate_cap bytes:
 * 10-byte header + body + 8-byte CRC-32/ISIZE trailer. */
size_t payload_gzip_stream_cap(size_t deflate_cap);

/* Base64 text cap (including the NUL terminator) for a binary stream of
 * stream_len bytes. */
size_t payload_gzip_base64_cap(size_t stream_len);

/* CRC-32 (IEEE 802.3, reflected — the gzip/zlib polynomial). Seed with 0 and
 * chain the return value for incremental use. */
uint32_t payload_gzip_crc32(uint32_t crc, const void *buf, size_t len);

/* Standard base64 (RFC 4648, '+'/'/', '=' padding, no line breaks). Returns
 * false when out_cap is too small; out is always NUL-terminated on success and
 * *out_len receives the text length. */
bool payload_gzip_base64_encode(const uint8_t *src, size_t src_len,
                                char *out, size_t out_cap, size_t *out_len);

/* One-call transport encoding: base64(gzip(src)). scratch receives the
 * intermediate gzip stream and must hold payload_gzip_stream_cap(
 * payload_gzip_deflate_bound(src_len)) bytes; out must hold
 * payload_gzip_base64_cap of the resulting stream (sizing with the same bound
 * is always sufficient). Fails closed — false on any deflate or capacity
 * error, with no partial output contract. */
bool payload_gzip_encode_base64(const uint8_t *src, size_t src_len,
                                payload_gzip_deflate_fn deflate, void *deflate_ctx,
                                uint8_t *scratch, size_t scratch_cap,
                                char *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
