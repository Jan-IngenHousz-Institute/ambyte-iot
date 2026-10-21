/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "script_update.h"
#include "sched_runner.h"
#include "sched_spec.h"
#include "esp_app_desc.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *root, *scenario;
static unsigned suspended, resumed, renames, writes, cut_counter;
static bool running = true;
static bool cuts_enabled;
static sched_identity_t loaded;
typedef struct { char sha[65], version[32], fw[32], installed[32]; } metadata_t;
static metadata_t metadata, pending_metadata;
esp_err_t install_boot_read(char **bytes, size_t *length);

static void cut(void)
{
    const char *at = getenv("SVC_INSTALL_CUT");
    if (cuts_enabled && at && ++cut_counter == strtoul(at, NULL, 10)) _exit(86);
}
static const char *path(const char *name, char out[1024])
{
    /* Any capture/event-store/SD dependency fails the harness at the boundary. */
    assert(!strncmp(name, "/littlefs/", 10));
    assert(!strcmp(name + 10, "schedule.yaml") || !strcmp(name + 10, "schedule.yaml.bak") ||
           !strcmp(name + 10, "svc/stage.bin"));
    assert(snprintf(out, 1024, "%s/%s", root, name + 10) < 1024);
    return out;
}
FILE *__real_fopen(const char *, const char *);
int __real_open(const char *, int, ...);
int __wrap_open(const char *name, int flags, ...)
{
    /* A2's heap-bounded digest now uses open/read rather than stdio. Keep
     * exercising that production path against the same isolated littlefs. */
    assert(flags == O_RDONLY);
    char mapped[1024]; return __real_open(path(name, mapped), flags);
}
FILE *__wrap_fopen(const char *name, const char *mode)
{
    char mapped[1024]; return __real_fopen(path(name, mapped), mode);
}
int __real_stat(const char *, struct stat *);
int __wrap_stat(const char *name, struct stat *st)
{
    char mapped[1024]; return __real_stat(path(name, mapped), st);
}
int __real_remove(const char *);
int __wrap_remove(const char *name)
{
    char mapped[1024]; cut(); int rc = __real_remove(path(name, mapped)); cut(); return rc;
}
int __real_rename(const char *, const char *);
int __wrap_rename(const char *from, const char *to)
{
    char a[1024], b[1024]; ++renames; cut();
    bool install = !strcmp(from, SCRIPT_UPDATE_SERVICE_STAGING_PATH);
    if (install && (!strcmp(scenario, "rename_error") || !strcmp(scenario, "rollback_error"))) { errno = EIO; return -1; }
    if (!strcmp(from, "/littlefs/schedule.yaml.bak") && !strcmp(scenario, "rollback_error")) { errno = EIO; return -1; }
    int rc = __real_rename(path(from, a), path(to, b)); cut(); return rc;
}
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = {.version = "test-firmware"}; return &app;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    assert(!strcmp(ns, "script_upd"));
    if (mode == NVS_READWRITE) pending_metadata = metadata;
    *out = 1; return ESP_OK;
}
static char *key(metadata_t *m, const char *name, size_t *capacity)
{
    if (!strcmp(name, "script_sha")) { *capacity = sizeof m->sha; return m->sha; }
    if (!strcmp(name, "script_ver")) { *capacity = sizeof m->version; return m->version; }
    if (!strcmp(name, "built_fw")) { *capacity = sizeof m->fw; return m->fw; }
    assert(!strcmp(name, "install_fw")); *capacity = sizeof m->installed; return m->installed;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *name, char *out, size_t *capacity)
{
    assert(h == 1); size_t size; const char *value = key(&metadata, name, &size);
    if (!*value) return ESP_ERR_NVS_NOT_FOUND;
    assert(strlen(value) + 1 <= *capacity); strcpy(out, value); *capacity = strlen(value) + 1; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *name, const char *value)
{
    assert(h == 1); size_t size; char *out = key(&pending_metadata, name, &size);
    assert(strlen(value) < size); strcpy(out, value); ++writes; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == 1); cut();
    if (!strcmp(scenario, "nvs_error")) return ESP_FAIL;
    metadata = pending_metadata;
    char file[1024]; snprintf(file, sizeof file, "%s/metadata", root);
    FILE *f = __real_fopen(file, "wb"); assert(f);
    assert(fwrite(&metadata, 1, sizeof metadata, f) == sizeof metadata);
    assert(!fflush(f) && !fsync(fileno(f)) && !fclose(f)); cut(); return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h == 1); }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "stub-error"; }
void mbedtls_sha256_init(mbedtls_sha256_context *c) { c->ctx = EVP_MD_CTX_new(); }
int mbedtls_sha256_starts(mbedtls_sha256_context *c, int n)
{ assert(!n); return EVP_DigestInit_ex(c->ctx, EVP_sha256(), NULL) == 1 ? 0 : -1; }
int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *b, size_t n)
{ return EVP_DigestUpdate(c->ctx, b, n) == 1 ? 0 : -1; }
int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char out[32])
{ return EVP_DigestFinal_ex(c->ctx, out, NULL) == 1 ? 0 : -1; }
void mbedtls_sha256_free(mbedtls_sha256_context *c) { EVP_MD_CTX_free(c->ctx); }
static void suspend(void) { ++suspended; if (strcmp(scenario, "stop_error")) running = false; }
static void resume(void)
{
    ++resumed; cut();
    if (!strcmp(scenario, "stale_loaded")) { running = true; return; }
    char *text = NULL, error[160]; size_t n = 0;
    if (install_boot_read(&text, &n) != ESP_OK) {
        running = true; memset(&loaded, 0, sizeof loaded);
        loaded.kind = SCHED_SOURCE_EMBEDDED_DEFAULT; return;
    }
    sched_program_t *program = malloc(sizeof *program); assert(program);
    if (sched_compile_text(text, n, program, error, sizeof error) != ESP_OK) {
        loaded.kind = SCHED_SOURCE_EMBEDDED_FALLBACK; running = true;
    } else {
        memset(&loaded, 0, sizeof loaded); loaded.generation = resumed;
        loaded.kind = SCHED_SOURCE_INSTALLED;
        unsigned char digest[32]; unsigned size;
        assert(EVP_Digest(text, n, digest, &size, EVP_sha256(), NULL) == 1 && size == 32);
        for (unsigned i = 0; i < 32; ++i) sprintf(loaded.sha256 + i * 2, "%02x", digest[i]);
        const char *workbook = sched_pool_str(program, program->workbook_version_id_off);
        if (workbook) { loaded.has_workbook = true; strcpy(loaded.workbook, workbook); }
        running = true;
        if (!strcmp(scenario, "fallback")) loaded.kind = SCHED_SOURCE_EMBEDDED_FALLBACK;
    }
    free(program); free(text); cut();
}
bool sched_runner_is_running(void) { return running; }
esp_err_t sched_runner_identity(sched_identity_t *out) { *out = loaded; return ESP_OK; }

int main(int argc, char **argv)
{
    assert(argc == 6); root = argv[1]; scenario = argv[2];
    char file[1024]; snprintf(file, sizeof file, "%s/metadata", root);
    FILE *f = __real_fopen(file, "rb");
    if (f) { assert(fread(&metadata, 1, sizeof metadata, f) == sizeof metadata); assert(!fclose(f)); }
    script_update_config_t config = {.workload_suspend = suspend, .workload_resume = resume};
    assert(script_update_init(&config) == ESP_OK);
    script_service_descriptor_t d = {.source_path = SCRIPT_UPDATE_SERVICE_STAGING_PATH,
        .sha256 = argv[3], .size = strtoul(argv[4], NULL, 10), .workbook = argv[5],
        .version = "a3", .built_fw = "test"};
    /* Establish a real compiled loaded-program boundary before install. */
    cuts_enabled = !strcmp(scenario, "boot_cut");
    const char *saved_scenario = scenario; scenario = "initial"; resume(); scenario = saved_scenario;
    resumed = 0; cut_counter = 0; cuts_enabled = true;
    if (!strcmp(scenario, "already_active")) {
        strcpy(metadata.sha, d.sha256); strcpy(metadata.version, d.version); strcpy(metadata.fw, d.built_fw);
    }
    bool applied = false;
    const char *error = !strcmp(scenario, "check") || !strcmp(scenario, "boot_cut") ? NULL : script_update_service_apply(&d, &applied);
    printf("{\"reason\":\"%s\",\"applied\":%s,\"verified\":%s,\"suspended\":%u,\"resumed\":%u,\"renames\":%u,\"writes\":%u,\"loaded_source\":\"%s\",\"loaded_sha\":\"%s\"}\n",
           error ? error : "", applied ? "true" : "false", script_update_service_matches(&d) ? "true" : "false",
           suspended, resumed, renames, writes,
           loaded.kind == SCHED_SOURCE_INSTALLED ? "INSTALLED" : "EMBEDDED_DEFAULT", loaded.sha256);
}
