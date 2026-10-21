#pragma once
#include <stddef.h>
/* Host crypto boundary uses libcrypto; the production file reader and NVS
 * trust decisions are compiled without extraction or duplication. */
typedef struct { void *ctx; } mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *ctx);
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224);
int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *buf, size_t n);
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char out[32]);
void mbedtls_sha256_free(mbedtls_sha256_context *ctx);
int mbedtls_sha256(const unsigned char *buf, size_t n, unsigned char out[32], int is224);
