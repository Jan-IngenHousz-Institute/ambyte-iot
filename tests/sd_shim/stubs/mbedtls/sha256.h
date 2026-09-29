#ifndef SD_SHIM_MBEDTLS_SHA256_H
#define SD_SHIM_MBEDTLS_SHA256_H
/* Host stand-in for the mbedtls SHA-256 streaming API (FIPS 180-4). */
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *c);
void mbedtls_sha256_free(mbedtls_sha256_context *c);
int  mbedtls_sha256_starts(mbedtls_sha256_context *c, int is224);
int  mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *p, size_t n);
int  mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char out[32]);
int  mbedtls_sha256(const unsigned char *p, size_t n, unsigned char out[32], int is224);
#endif
