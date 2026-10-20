#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "script_update.h"

static unsigned file_reads, nvs_reads;
static unsigned scratch_heap_allocations;
static size_t peak_stdio_buffer;

void *__real_malloc(size_t n);
void *__wrap_malloc(size_t n)
{
    scratch_heap_allocations++;
    return __real_malloc(n);
}

size_t __real_fread(void *buf, size_t size, size_t n, FILE *f);
size_t __wrap_fread(void *buf, size_t size, size_t n, FILE *f)
{
    size_t got = __real_fread(buf, size, n, f);
    size_t buffered = __fbufsize(f);
    if (buffered > peak_stdio_buffer) peak_stdio_buffer = buffered;
    return got;
}

FILE *__real_fopen(const char *, const char *);
FILE *__wrap_fopen(const char *path, const char *mode)
{
    assert(!strcmp(path, "/littlefs/schedule.yaml"));
    assert(!strcmp(mode, "rb"));
    file_reads++;
    return __real_fopen(getenv("A2_SCHEDULE_FILE"), mode);
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    assert(!strcmp(ns, "script_upd") && mode == NVS_READONLY);
    nvs_reads++;
    if (getenv("A2_NVS_FAIL")) return ESP_ERR_NOT_FOUND;
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h == 1); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *n)
{
    assert(h == 1);
    const char *env = !strcmp(key, "script_sha") ? "A2_STORED_SHA" :
                      !strcmp(key, "script_ver") ? "A2_RELEASE_VERSION" :
                      !strcmp(key, "built_fw") ? "A2_BUILT_FW" :
                      !strcmp(key, "install_fw") ? "A2_INSTALLED_FW" : NULL;
    assert(env != NULL);
    const char *v = getenv(env);
    if (!v) return ESP_ERR_NVS_NOT_FOUND;
    assert(strlen(v) + 1 <= *n);
    memcpy(out, v, strlen(v) + 1);
    *n = strlen(v) + 1;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{ (void)h; (void)k; (void)v; abort(); }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; abort(); }

void mbedtls_sha256_init(mbedtls_sha256_context *c) { c->ctx = EVP_MD_CTX_new(); }
int mbedtls_sha256_starts(mbedtls_sha256_context *c, int is224)
{ assert(!is224); return EVP_DigestInit_ex(c->ctx, EVP_sha256(), NULL) == 1 ? 0 : -1; }
int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *buf, size_t n)
{ return EVP_DigestUpdate(c->ctx, buf, n) == 1 ? 0 : -1; }
int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char out[32])
{ return EVP_DigestFinal_ex(c->ctx, out, NULL) == 1 ? 0 : -1; }
void mbedtls_sha256_free(mbedtls_sha256_context *c) { EVP_MD_CTX_free(c->ctx); }

int main(void)
{
    script_identity_t out;
    esp_err_t rc = script_update_get_identity(&out);
    assert(file_reads == 1);
    assert(nvs_reads <= 1);
    if (getenv("A2_CHECK_READ_MEMORY")) {
        fprintf(stderr, "hash read: scratch heap allocations=%u stdio buffer=%zu\n",
                scratch_heap_allocations, peak_stdio_buffer);
        assert(scratch_heap_allocations == 0 && peak_stdio_buffer <= 1);
    }
    printf("rc=%d\nsha=%s\nversion=%s\nbuilt=%s\ninstalled=%s\nverified=%d\n",
           rc, out.sha256, out.version, out.built_against_fw,
           out.installed_on_fw, out.release_metadata_verified);
    return 0;
}
