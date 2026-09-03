#include "payload_gzip.h"

#include <string.h>

/* RFC 1951 stored blocks cap LEN at 65,535 bytes; each block costs a 1-byte
 * BFINAL/BTYPE header plus the 4-byte LEN/NLEN pair. Even a Huffman-coded
 * final/EOB tail stays far inside the extra 16-byte slack, so this bound holds
 * for any conforming deflate provider, not just the stored-block worst case. */
#define GZIP_STORED_BLOCK_MAX 65535U
#define GZIP_HEADER_LEN 10U
#define GZIP_TRAILER_LEN 8U

size_t payload_gzip_deflate_bound(size_t src_len)
{
    size_t blocks = src_len / GZIP_STORED_BLOCK_MAX + 1U;
    return src_len + blocks * 5U + 16U;
}

size_t payload_gzip_stream_cap(size_t deflate_cap)
{
    return GZIP_HEADER_LEN + deflate_cap + GZIP_TRAILER_LEN;
}

size_t payload_gzip_base64_cap(size_t stream_len)
{
    return ((stream_len + 2U) / 3U) * 4U + 1U;
}

/* Nibble-table CRC-32: 64 bytes of const table instead of the usual 1 KiB,
 * with no lazy runtime initialisation to reason about. Throughput is ample for
 * one <=64 KiB envelope per publish. */
static const uint32_t crc32_nibble[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
};

uint32_t payload_gzip_crc32(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ crc32_nibble[crc & 0x0FU];
        crc = (crc >> 4) ^ crc32_nibble[crc & 0x0FU];
    }
    return ~crc;
}

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool payload_gzip_base64_encode(const uint8_t *src, size_t src_len,
                                char *out, size_t out_cap, size_t *out_len)
{
    if (out == NULL || out_cap == 0 || (src == NULL && src_len != 0)) return false;
    size_t need = payload_gzip_base64_cap(src_len);
    if (out_cap < need) return false;

    size_t o = 0;
    size_t i = 0;
    for (; i + 3U <= src_len; i += 3U) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1U] << 8) | src[i + 2U];
        out[o++] = b64_alphabet[(v >> 18) & 0x3FU];
        out[o++] = b64_alphabet[(v >> 12) & 0x3FU];
        out[o++] = b64_alphabet[(v >> 6) & 0x3FU];
        out[o++] = b64_alphabet[v & 0x3FU];
    }
    size_t rem = src_len - i;
    if (rem == 1U) {
        uint32_t v = (uint32_t)src[i] << 16;
        out[o++] = b64_alphabet[(v >> 18) & 0x3FU];
        out[o++] = b64_alphabet[(v >> 12) & 0x3FU];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2U) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1U] << 8);
        out[o++] = b64_alphabet[(v >> 18) & 0x3FU];
        out[o++] = b64_alphabet[(v >> 12) & 0x3FU];
        out[o++] = b64_alphabet[(v >> 6) & 0x3FU];
        out[o++] = '=';
    }
    out[o] = '\0';
    if (out_len != NULL) *out_len = o;
    return true;
}

bool payload_gzip_encode_base64(const uint8_t *src, size_t src_len,
                                payload_gzip_deflate_fn deflate, void *deflate_ctx,
                                uint8_t *scratch, size_t scratch_cap,
                                char *out, size_t out_cap, size_t *out_len)
{
    if (src == NULL || deflate == NULL || scratch == NULL || out == NULL) return false;
    if (scratch_cap < GZIP_HEADER_LEN + GZIP_TRAILER_LEN) return false;

    /* RFC 1952 header: magic, CM=8 (deflate), no flags, MTIME=0 (the envelope
     * already carries the measurement timestamp), XFL=0, OS=255 (unknown). */
    static const uint8_t header[GZIP_HEADER_LEN] = {
        0x1FU, 0x8BU, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFFU,
    };
    memcpy(scratch, header, GZIP_HEADER_LEN);

    size_t body_len = 0;
    if (!deflate(deflate_ctx, src, src_len, scratch + GZIP_HEADER_LEN,
                 scratch_cap - GZIP_HEADER_LEN - GZIP_TRAILER_LEN, &body_len)) {
        return false;
    }
    if (body_len > scratch_cap - GZIP_HEADER_LEN - GZIP_TRAILER_LEN) return false;

    uint32_t crc = payload_gzip_crc32(0, src, src_len);
    uint32_t isize = (uint32_t)src_len; /* mod 2^32 per RFC 1952 */
    uint8_t *trailer = scratch + GZIP_HEADER_LEN + body_len;
    trailer[0] = (uint8_t)(crc & 0xFFU);
    trailer[1] = (uint8_t)((crc >> 8) & 0xFFU);
    trailer[2] = (uint8_t)((crc >> 16) & 0xFFU);
    trailer[3] = (uint8_t)((crc >> 24) & 0xFFU);
    trailer[4] = (uint8_t)(isize & 0xFFU);
    trailer[5] = (uint8_t)((isize >> 8) & 0xFFU);
    trailer[6] = (uint8_t)((isize >> 16) & 0xFFU);
    trailer[7] = (uint8_t)((isize >> 24) & 0xFFU);

    size_t stream_len = GZIP_HEADER_LEN + body_len + GZIP_TRAILER_LEN;
    return payload_gzip_base64_encode(scratch, stream_len, out, out_cap, out_len);
}
