#include "script_update.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "fleet_jitter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sched_runner.h"
#include "sched_spec.h"

#define TAG "script_upd"

#define SCRIPT_ID_MAX       64
#define SCRIPT_REBOOT_DELAY_MS 500   /* let the 'applied' reply flush before esp_restart (matches ota_update) */

/* Canonical schedule home is internal flash alongside the internal event store:
 * schedule delivery is flashing/OTA, and a remote push must never depend on the
 * corruption-prone archive card. Must match sched_runner's installed path. */
#define SCHEDULE_PATH       "/littlefs/schedule.yaml"
/* Public so the console's `schedule put` stages into the same file this installs
 * from; one definition, not two that can drift. */
#define SCHEDULE_PATH_NEW   SCRIPT_UPDATE_STAGING_PATH
#define SCHEDULE_PATH_BAK   "/littlefs/schedule.yaml.bak"

#define NVS_NS         "script_upd"
#define KEY_APPLIED    "applied_id"
#define KEY_SCRIPT_SHA  "script_sha"
#define KEY_SCRIPT_VER  "script_ver"
#define KEY_BUILT_FW    "built_fw"
#define KEY_INSTALL_FW  "install_fw"

#define OP_UPDATE       0
#define OP_UPDATE_URL   2
/* Bytes already staged by the console (`schedule begin`/`schedule put`),
 * so the board needs no network to install a script. */
#define OP_UPDATE_LOCAL 3

/* The status reply is best-effort: the serial log is authoritative, and an
 * operator is usually watching it. This used to be 3000 (five minutes), which on
 * a board that cannot reach its broker meant every failed URL install sat here
 * burning the caller's deadline after the real work had already finished. */
#define SCRIPT_RECONNECT_WAIT_TICKS 150   /* 15 s at 100 ms per tick */

#define SCRIPT_DL_BUF          4096     /* HTTP chunk size — small on purpose (no large contiguous alloc) */
#define SCRIPT_HTTP_TIMEOUT_MS 20000
#define SCRIPT_FLEET_JITTER_SLOTS 900U   /* one-second slots: 0:00 through 14:59 */

typedef struct {
    uint8_t op;
    bool    reboot;            /* update ops: reboot after a successful swap when requested */
    bool    fleet_jitter;       /* OP_UPDATE_URL only: stagger remotely triggered fleet work */
    char    id[SCRIPT_ID_MAX];
    char    checksum[65];      /* optional sha256 hex ('\0' = absent) */
    char    script_version[SCRIPT_IDENTITY_VERSION_LEN];
    char    built_against_fw[SCRIPT_IDENTITY_VERSION_LEN];
    char   *text;              /* heap-dup'd script/snippet — freed by the worker */
} script_req_t;

static script_update_config_t s_cfg;
static bool                   s_ready;   /* init done; dispatch via the shared worker (s_cfg.submit) */

/* ── dedupe latch (NVS, success only — same semantics as the OTAs) ────────── */

static void identity_set(const script_req_t *r, const char *sha256)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    const esp_app_desc_t *app = esp_app_get_description();
    const char *installed_on = app != NULL ? app->version : "";

    /* The retained-command dedupe is a reboot-safety invariant, not telemetry.
     * Commit it independently before provenance: a full NVS must never let one
     * optional identity key suppress KEY_APPLIED and create a reboot loop. */
    esp_err_t latch_err = ESP_OK;
    if (r->id[0] != '\0') {
        latch_err = nvs_set_str(h, KEY_APPLIED, r->id);
        if (latch_err == ESP_OK) latch_err = nvs_commit(h);
        if (latch_err != ESP_OK) {
            ESP_LOGE(TAG, "could not persist script dedupe latch: %s",
                     esp_err_to_name(latch_err));
        }
    }

    /* Keep the provenance tuple atomic. In particular, never commit a new SHA
     * alongside an old version if one of the later writes runs out of space. */
    esp_err_t err = nvs_set_str(h, KEY_SCRIPT_SHA, sha256 != NULL ? sha256 : "");
    if (err == ESP_OK) err = nvs_set_str(h, KEY_SCRIPT_VER, r->script_version);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_BUILT_FW, r->built_against_fw);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_INSTALL_FW, installed_on);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "script applied but provenance was not persisted: %s",
                 esp_err_to_name(err));
    }
    nvs_close(h);
}

static bool already_applied(const char *id)
{
    if (id == NULL || id[0] == '\0') return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char prev[SCRIPT_ID_MAX] = "";
    size_t len = sizeof prev;
    esp_err_t err = nvs_get_str(h, KEY_APPLIED, prev, &len);
    nvs_close(h);
    return err == ESP_OK && strcmp(prev, id) == 0;
}

/* ── reporting ────────────────────────────────────────────────────────────── */

static void json_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    if (in == NULL) { if (cap) out[0] = '\0'; return; }
    for (const char *p = in; *p != '\0' && o + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n')        { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\t')        { out[o++] = '\\'; out[o++] = 't'; }
        else if (c >= 0x20)        { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

static void publish_json(const char *msg, int n)
{
    if (s_cfg.publish == NULL || s_cfg.status_topic == NULL || s_cfg.status_topic[0] == '\0') {
        return;
    }
    if (s_cfg.is_connected != NULL && !s_cfg.is_connected()) {
        return;   /* console log is authoritative; MQTT report is best-effort */
    }
    if (n > 0) {
        int msg_id = 0;
        s_cfg.publish(s_cfg.status_topic, msg, (size_t)n, &msg_id);
    }
}

static void report_script_impl(const char *state, const char *id,
                               const char *detail, bool installed_after_swap)
{
    char esc_id[SCRIPT_ID_MAX * 2 + 1] = "", esc_detail[192] = "";
    char esc_sha[129] = "", esc_ver[65] = "", esc_built[65] = "", esc_installed[65] = "";
    char esc_fw[65] = "";
    script_identity_t identity = {0};
    bool identity_ok = script_update_get_identity(&identity) == ESP_OK;
    const esp_app_desc_t *app = esp_app_get_description();
    sched_source_t source = {0};
    sched_runner_source(&source);
    /* Rebooting installs report after a validated file swap while the runner's
     * in-memory source still names the previous program. Only those explicit
     * call sites override it: a checksum-less id-only dedupe may also report
     * `applied` while stopped, but has not verified any installed file. */
    if (installed_after_swap) {
        source.kind = SCHED_SOURCE_INSTALLED;
    }
    static const char *const source_names[] = {
        "none", "installed", "embedded_default", "embedded_fallback",
    };
    json_escape(esc_id, sizeof esc_id, id);
    json_escape(esc_detail, sizeof esc_detail, detail);
    json_escape(esc_sha, sizeof esc_sha, identity.sha256);
    json_escape(esc_ver, sizeof esc_ver, identity.version);
    json_escape(esc_built, sizeof esc_built, identity.built_against_fw);
    json_escape(esc_installed, sizeof esc_installed, identity.installed_on_fw);
    json_escape(esc_fw, sizeof esc_fw, app != NULL ? app->version : "");
    char msg[1024];
    int n = snprintf(msg, sizeof msg,
        "{\"type\":\"script_status\",\"device_id\":\"%s\",\"id\":\"%s\",\"state\":\"%s\""
        ",\"app_version\":\"%s\",\"script_sha256\":\"%s\",\"script_version\":\"%s\""
        ",\"script_built_against_fw\":\"%s\",\"script_installed_on_fw\":\"%s\""
        ",\"script_metadata_verified\":%s,\"schedule_source\":\"%s\"%s%s%s}",
        s_cfg.device_id ? s_cfg.device_id : "", esc_id, state,
        esc_fw, identity_ok ? esc_sha : "", identity_ok ? esc_ver : "",
        identity_ok ? esc_built : "", identity_ok ? esc_installed : "",
        identity_ok && identity.release_metadata_verified ? "true" : "false",
        source_names[source.kind <= SCHED_SOURCE_EMBEDDED_FALLBACK ? source.kind : 0],
        detail ? ",\"detail\":\"" : "", esc_detail, detail ? "\"" : "");
    if (n > 0 && (size_t)n < sizeof msg) publish_json(msg, n);
}

static void report_script(const char *state, const char *id, const char *detail)
{
    report_script_impl(state, id, detail, false);
}

static void report_script_installed(const char *state, const char *id,
                                    const char *detail)
{
    report_script_impl(state, id, detail, true);
}

/* Keep the maintenance gate/worker reserved during the stable per-device slot.
 * MQTT and measurements remain active until the URL handler quiesces below. */
static void wait_for_fleet_slot(void)
{
    uint32_t delay_s = 0;
    esp_err_t err = fleet_jitter_slot_for_sta_mac(SCRIPT_FLEET_JITTER_SLOTS,
                                                   &delay_s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA MAC unavailable for fleet script jitter: %s — using 0 s",
                 esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "fleet script URL delay: %lu s", (unsigned long)delay_s);
    if (delay_s > 0U) {
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000U));
    }
}

/* ── OP_UPDATE: replace the internal schedule.yaml ────────────────────────────── */

/* SHA-256(text) == checksum (hex, case-insensitive)? */
static bool sha256_text(const char *text, char hex[65])
{
    unsigned char digest[32];
    if (mbedtls_sha256((const unsigned char *)text, strlen(text), digest, 0) != 0) {
        return false;
    }
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    hex[64] = '\0';
    return true;
}

/* Byte length of `path`, or -1 when it cannot be sized (missing/unreadable). */
static long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static esp_err_t sha256_file(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return ESP_ERR_NOT_FOUND;
    uint8_t *buf = malloc(1024);
    if (buf == NULL) { fclose(f); return ESP_ERR_NO_MEM; }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    esp_err_t err = ESP_OK;
    while (1) {
        size_t n = fread(buf, 1, 1024, f);
        if (n > 0) mbedtls_sha256_update(&sha, buf, n);
        if (n < 1024) {
            if (ferror(f)) err = ESP_FAIL;
            break;
        }
    }
    unsigned char digest[32];
    if (err == ESP_OK) mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    free(buf);
    fclose(f);
    if (err != ESP_OK) return err;
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    hex[64] = '\0';
    return ESP_OK;
}

static void nvs_read_string(nvs_handle_t h, const char *key, char *out, size_t out_cap)
{
    if (out_cap == 0) return;
    out[0] = '\0';
    size_t len = out_cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
}

esp_err_t script_update_get_identity(script_identity_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    /* schedule.yaml is on internal littlefs — no unmount can free the volume
     * mid-read, so the old SD RW-gate (audit R-6) is unnecessary here. */
    esp_err_t err = sha256_file(SCHEDULE_PATH, out->sha256);
    if (err != ESP_OK) return err;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK; /* exact hash is still useful without release metadata */
    }
    char stored_sha[65] = "";
    nvs_read_string(h, KEY_SCRIPT_SHA, stored_sha, sizeof stored_sha);
    if (strlen(stored_sha) == 64 && strncasecmp(stored_sha, out->sha256, 64) == 0) {
        nvs_read_string(h, KEY_SCRIPT_VER, out->version, sizeof out->version);
        nvs_read_string(h, KEY_BUILT_FW, out->built_against_fw, sizeof out->built_against_fw);
        nvs_read_string(h, KEY_INSTALL_FW, out->installed_on_fw, sizeof out->installed_on_fw);
        /* Legacy pushes record the exact digest but carry no release tuple.
         * Keep those distinguishable from a manifest-backed schedule release. */
        if (out->version[0] != '\0' && out->built_against_fw[0] != '\0') {
            out->release_metadata_verified = true;
        } else {
            out->version[0] = '\0';
            out->built_against_fw[0] = '\0';
            out->installed_on_fw[0] = '\0';
        }
    }
    nvs_close(h);
    return ESP_OK;
}

/* A release command is a duplicate only when both its success latch and its
 * authoritative byte identity still match. Legacy commands without a checksum
 * retain id-only dedupe so retained reboot commands cannot loop. This runs on
 * the maintenance worker, never the MQTT callback task. */
static bool request_already_active(const script_req_t *r)
{
    if (!already_applied(r->id)) return false;
    if (r->checksum[0] == '\0') return true;

    char active_sha[65] = "";
    esp_err_t err = sha256_file(SCHEDULE_PATH, active_sha);
    if (err != ESP_OK) return false;
    return strncasecmp(active_sha, r->checksum, 64) == 0;
}

/* Compile into heap scratch state so install validation and runner loading use
 * the same parser, action catalog and semantic checks without growing this
 * maintenance worker's stack by sizeof(sched_program_t). */
static bool schedule_ok(const char *text, size_t len, char *err, size_t err_cap)
{
    sched_program_t *scratch = malloc(sizeof(*scratch));
    if (scratch == NULL) {
        snprintf(err, err_cap, "out of memory for schedule validation");
        return false;
    }
    bool ok = sched_compile_text(text, len, scratch, err, err_cap) == ESP_OK;
    free(scratch);
    return ok;
}

static bool sched_spec_compile_file(const char *path, char *err, size_t err_cap)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(err, err_cap, "%s: %s", path, strerror(errno));
        return false;
    }
    char *buf = malloc(SCHED_YAML_MAX_FILE_BYTES + 1);
    if (buf == NULL) {
        fclose(f);
        snprintf(err, err_cap, "out of memory for schedule validation");
        return false;
    }
    size_t len = fread(buf, 1, SCHED_YAML_MAX_FILE_BYTES + 1, f);
    bool ok = !ferror(f) && len <= SCHED_YAML_MAX_FILE_BYTES;
    fclose(f);
    if (!ok) {
        if (len > SCHED_YAML_MAX_FILE_BYTES) {
            snprintf(err, err_cap, "schedule exceeds %u-byte limit",
                     (unsigned)SCHED_YAML_MAX_FILE_BYTES);
        } else {
            snprintf(err, err_cap, "schedule read failed");
        }
        free(buf);
        return false;
    }
    ok = schedule_ok(buf, len, err, err_cap);
    free(buf);
    return ok;
}

/* Stream `url` (HTTPS) into `path`, hashing the bytes as they arrive. 4 KB chunks
 * — never a large contiguous alloc, so this succeeds where an inline 16 KB MQTT
 * message fails on a fragmented heap. On success writes the lowercase-hex SHA-256
 * into hex65[65] and the byte count into *out_size. Removes a partial file on
 * error. Same TLS/buffer settings as the OTA downloaders (cert bundle validates
 * GitHub + CDN; 4 KiB buffers fit long signed-redirect URLs). */
static esp_err_t download_to_file_sha256(const char *url, const char *path,
                                         char hex65[65], size_t *out_size)
{
    *out_size = 0;
    hex65[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = SCRIPT_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .buffer_size       = SCRIPT_DL_BUF,
        .buffer_size_tx    = SCRIPT_DL_BUF,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return ESP_FAIL;

    esp_err_t err = ESP_OK;
    int64_t clen = -1;
    int status = -1;
    /* The native open/read streaming API does not run the blocking client's
     * automatic redirect loop. GitHub release assets always answer with a 302
     * to release-assets.githubusercontent.com, so follow redirects explicitly
     * before opening the destination file. */
    for (int redirects = 0; redirects <= 5; redirects++) {
        err = esp_http_client_open(c, 0);
        if (err != ESP_OK) break;
        clen = esp_http_client_fetch_headers(c);
        if (clen < 0) {
            err = ESP_FAIL;
            break;
        }
        status = esp_http_client_get_status_code(c);
        if (status < 300 || status >= 400) break;
        if (redirects == 5) {
            err = ESP_ERR_HTTP_MAX_REDIRECT;
            break;
        }
        err = esp_http_client_set_redirection(c);
        esp_http_client_close(c);
        if (err != ESP_OK) break;
    }
    if (err != ESP_OK) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "download HTTP status %d (need 200 — use a direct raw URL, not a /blob/ page)",
                 status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    if (clen > SCHED_YAML_MAX_FILE_BYTES) {
        ESP_LOGE(TAG, "download is %lld bytes; schedule limit is %u",
                 (long long)clen, (unsigned)SCHED_YAML_MAX_FILE_BYTES);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    uint8_t *buf = malloc(SCRIPT_DL_BUF);
    if (buf == NULL) {
        fclose(f);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        remove(path);
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);   /* 0 = SHA-256 */

    size_t total = 0;
    bool wr_ok = true;
    while (1) {
        int r = esp_http_client_read(c, (char *)buf, SCRIPT_DL_BUF);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) break;   /* EOF */
        if ((size_t)r > SCHED_YAML_MAX_FILE_BYTES - total) {
            ESP_LOGE(TAG, "chunked download exceeds %u-byte schedule limit",
                     (unsigned)SCHED_YAML_MAX_FILE_BYTES);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)r);
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { err = ESP_ERR_NO_MEM; wr_ok = false; break; }
        total += (size_t)r;
        vTaskDelay(1);       /* yield so the idle task is fed on a fast link */
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (wr_ok) { fflush(f); fsync(fileno(f)); }
    free(buf);
    fclose(f);

    /* Positive completion check: esp_http_client_read()==0 can't distinguish a
     * real EOF from a mid-stream TLS/socket close, and the Content-Length guard
     * below is skipped for chunked responses (clen<=0). Without this, a truncated
     * download whose prefix happens to parse could be installed. */
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(c)) {
        ESP_LOGE(TAG, "incomplete download — connection closed before all data received");
        err = ESP_FAIL;
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK && clen > 0 && total != (size_t)clen) {
        ESP_LOGE(TAG, "short download: %u of %lld bytes", (unsigned)total, (long long)clen);
        err = ESP_FAIL;
    }
    if (err != ESP_OK) { remove(path); return err; }

    for (int i = 0; i < 32; i++) sprintf(hex65 + i * 2, "%02x", digest[i]);
    hex65[64] = '\0';
    *out_size = total;
    return ESP_OK;
}

static void do_update_impl(const script_req_t *r);

/* (The audit R-6 SD RW-gate wrapper is gone: the script now lives on internal
 * littlefs, whose volume is never freed at runtime.) */
static void do_update(const script_req_t *r)
{
    do_update_impl(r);
}

static void do_update_impl(const script_req_t *r)
{
    const size_t len = strlen(r->text);
    ESP_LOGW(TAG, "script_update id=%s: %u bytes", r->id[0] ? r->id : "(none)", (unsigned)len);
    char detail[160];
    char got[65];

    /* On-receipt ack: tell the operator the command was received before doing any
     * work (compile + staging write + runner stop can take a few seconds, and the
     * fleet-OTA notebook waits for an initial reply). MQTT stays up on the inline
     * path, so this lands immediately. */
    report_script("accepted", r->id, NULL);

    if (len > SCHED_YAML_MAX_FILE_BYTES) {
        snprintf(detail, sizeof detail, "schedule exceeds %u-byte limit",
                 (unsigned)SCHED_YAML_MAX_FILE_BYTES);
        report_script("failed", r->id, detail);
        return;
    }
    if (!sha256_text(r->text, got)) {
        report_script("failed", r->id, "sha256 failed");
        return;
    }
    if (r->checksum[0] != '\0' &&
        (strlen(r->checksum) != 64 || strncasecmp(got, r->checksum, 64) != 0)) {
        ESP_LOGE(TAG, "checksum mismatch — script rejected");
        report_script("failed", r->id, "sha256 mismatch");
        return;
    }
    if (!schedule_ok(r->text, len, detail, sizeof detail)) {
        ESP_LOGE(TAG, "schedule compile failed: %s — schedule.yaml untouched", detail);
        report_script("failed", r->id, detail);
        return;
    }
    /* Stage the new script next to the live one, fully flushed before any swap. */
    FILE *f = fopen(SCHEDULE_PATH_NEW, "wb");
    if (f == NULL) {
        report_script("failed", r->id, "cannot open " SCHEDULE_PATH_NEW);
        return;
    }
    bool wr_ok = (fwrite(r->text, 1, len, f) == len);
    if (wr_ok) { fflush(f); fsync(fileno(f)); }
    fclose(f);
    if (!wr_ok) {
        remove(SCHEDULE_PATH_NEW);
        report_script("failed", r->id, "staging write failed");
        return;
    }

    /* Stop the runner before the swap; a stop timeout means the script is wedged
     * in a long C call — leave everything untouched and let the operator retry. */
    if (sched_runner_stop(5000) == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "schedule task still busy — not swapping; retry in a moment");
        report_script("failed", r->id, "schedule task busy; retry");
        return;
    }

    /* Swap: the previous schedule survives as schedule.yaml.bak. rename() is
     * atomic on littlefs; a missing active file on first install is fine. */
    remove(SCHEDULE_PATH_BAK);
    (void)rename(SCHEDULE_PATH, SCHEDULE_PATH_BAK);
    if (rename(SCHEDULE_PATH_NEW, SCHEDULE_PATH) != 0) {
        ESP_LOGE(TAG, "rename to %s failed — restarting the old schedule", SCHEDULE_PATH);
        (void)rename(SCHEDULE_PATH_BAK, SCHEDULE_PATH);   /* best-effort restore */
        (void)sched_runner_start();
        report_script("failed", r->id, "rename failed");
        return;
    }

    /* Reboot path (default): the new schedule is in place and the runner is
     * already stopped — a full restart runs it from a fresh boot (clean heap,
     * ordered startup). Latch FIRST so a retained trigger dedupes on reconnect
     * and can't loop the reboot (same guard as ota_update). */
    if (r->reboot) {
        identity_set(r, got);
        ESP_LOGW(TAG, "schedule.yaml replaced (%u bytes); previous kept as %s — rebooting to run it",
                 (unsigned)len, SCHEDULE_PATH_BAK);
        snprintf(detail, sizeof detail, "%u bytes; rebooting", (unsigned)len);
        report_script_installed("applied", r->id, detail);
        vTaskDelay(pdMS_TO_TICKS(SCRIPT_REBOOT_DELAY_MS));   /* flush the MQTT reply */
        esp_restart();                                       /* no return */
    }

    /* In-place path (reboot=false): restart the schedule runner on the new file. */
    esp_err_t err = sched_runner_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runner restart failed: %s (schedule IS installed — `schedule start` manually)",
                 esp_err_to_name(err));
        report_script("failed", r->id, "script installed but runner restart failed");
        return;
    }

    identity_set(r, got);
    ESP_LOGW(TAG, "schedule.yaml replaced (%u bytes) + runner restarted; previous kept as %s",
             (unsigned)len, SCHEDULE_PATH_BAK);
    snprintf(detail, sizeof detail, "%u bytes", (unsigned)len);
    report_script("applied", r->id, detail);
}

/* ── OP_UPDATE_URL: download the internal schedule.yaml from a URL ─────────────────
 * The command message is tiny (just the URL), so it's received even on a
 * fragmented heap; the heavy transfer is a chunked HTTPS download after the runner is
 * stopped (heap defragmented). This is the reliable path for large scripts —
 * inline 16 KB MQTT delivery needs a contiguous TLS record buffer the fragmented
 * heap can't provide. `r->text` holds the URL. */
static void do_update_url_impl(const script_req_t *r);

/* (SD RW-gate wrapper removed — see do_update above.) */
static void do_update_url(const script_req_t *r)
{
    do_update_url_impl(r);
}

/* Everything after the bytes are staged at SCHEDULE_PATH_NEW: verify the digest,
 * compile, rotate .bak, swap atomically, record provenance. Shared by the
 * URL worker and the serial-push worker so the ONLY difference between them is
 * how schedule.yaml.new got written; the dangerous half has exactly one
 * implementation. Leaves schedule.yaml untouched on every failure path. */
static bool verify_and_swap_staged(const script_req_t *r, const char *got, size_t n,
                                   char *detail, size_t detail_cap)
{
    if (r->checksum[0] != '\0' &&
        (strlen(r->checksum) != 64 || strncasecmp(got, r->checksum, 64) != 0)) {
        ESP_LOGE(TAG, "checksum mismatch, script rejected");
        remove(SCHEDULE_PATH_NEW);
        snprintf(detail, detail_cap, "sha256 mismatch");
        return false;
    }
    if (!sched_spec_compile_file(SCHEDULE_PATH_NEW, detail, detail_cap)) {
        ESP_LOGE(TAG, "schedule compile failed: %s, schedule.yaml untouched", detail);
        remove(SCHEDULE_PATH_NEW);
        return false;
    }
    ESP_LOGW(TAG, "staged %u bytes, sha256=%s", (unsigned)n, got);
    /* Swap: previous schedule survives as schedule.yaml.bak (manual recovery). */
    remove(SCHEDULE_PATH_BAK);
    (void)rename(SCHEDULE_PATH, SCHEDULE_PATH_BAK);
    if (rename(SCHEDULE_PATH_NEW, SCHEDULE_PATH) != 0) {
        ESP_LOGE(TAG, "rename to %s failed", SCHEDULE_PATH);
        (void)rename(SCHEDULE_PATH_BAK, SCHEDULE_PATH);   /* best-effort restore */
        snprintf(detail, detail_cap, "rename failed");
        return false;
    }
    identity_set(r, got); /* before any reboot: dedupes retained trigger + records provenance */
    return true;
}

static void do_update_url_impl(const script_req_t *r)
{
    ESP_LOGW(TAG, "script_update(url) id=%s: %s", r->id[0] ? r->id : "(none)", r->text);
    char detail[192] = "";

    if (s_cfg.workload_suspend == NULL || s_cfg.workload_resume == NULL) {
        ESP_LOGE(TAG, "url variant needs workload hooks — not configured");
        report_script("failed", r->id, "url variant unavailable");
        return;
    }

    /* On-receipt ack — MUST publish BEFORE comms_suspend() below, after which MQTT
     * is gone and the operator would otherwise see nothing until the terminal
     * report minutes later (or never, if the reconnect drops it). Flush before the
     * comms drop, mirroring the OTA path's accepted-ack flush. */
    report_script("accepted", r->id, NULL);
    vTaskDelay(pdMS_TO_TICKS(SCRIPT_REBOOT_DELAY_MS));

    if (r->fleet_jitter) {
        wait_for_fleet_slot();
    } else {
        ESP_LOGI(TAG, "local script URL install: skipping fleet jitter");
    }

    /* Quiesce like the OTAs: stop the runner (frees buffers + UART, defragments)
     * AND stop MQTT (frees its TLS heap) so the download's HTTPS handshake gets a
     * clean, contiguous heap on this PSRAM-less board. MQTT is resumed before we
     * report. */
    s_cfg.workload_suspend();
    if (s_cfg.comms_suspend != NULL) s_cfg.comms_suspend();

    bool applied = false;
    size_t n = 0;
    char got[65] = "";

    {
        esp_err_t err = download_to_file_sha256(r->text, SCHEDULE_PATH_NEW, got, &n);
        if (err != ESP_OK) {
            snprintf(detail, sizeof detail, "download failed (%s)", esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", detail);
        } else {
            /* The runner is already stopped; rename() is atomic on littlefs. */
            applied = verify_and_swap_staged(r, got, n, detail, sizeof detail);
        }
    }

    /* Bring MQTT back and give it a moment to reconnect so the status reply lands
     * (best-effort — the serial log is authoritative either way). */
    if (s_cfg.comms_resume != NULL) {
        s_cfg.comms_resume();
        for (int i = 0; i < SCRIPT_RECONNECT_WAIT_TICKS && s_cfg.is_connected != NULL
                        && !s_cfg.is_connected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    if (!applied) {
        report_script("failed", r->id, detail);
        s_cfg.workload_resume();   /* restart the old script */
        return;
    }

    if (r->reboot) {
        ESP_LOGW(TAG, "schedule.yaml replaced from url (%u bytes); previous kept as %s — rebooting to run it",
                 (unsigned)n, SCHEDULE_PATH_BAK);
        snprintf(detail, sizeof detail, "%u bytes; rebooting", (unsigned)n);
        report_script_installed("applied", r->id, detail);
        vTaskDelay(pdMS_TO_TICKS(SCRIPT_REBOOT_DELAY_MS));   /* flush the MQTT reply */
        esp_restart();                                       /* no return */
    }

    /* In-place: restart the schedule runner on the new file. */
    s_cfg.workload_resume();
    ESP_LOGW(TAG, "schedule.yaml replaced from url (%u bytes) + runner restarted; previous kept as %s",
             (unsigned)n, SCHEDULE_PATH_BAK);
    snprintf(detail, sizeof detail, "%u bytes", (unsigned)n);
    report_script("applied", r->id, detail);
}

/* ── OP_UPDATE_LOCAL: install bytes the console already staged ─────────────── */

static void do_update_local_impl(const script_req_t *r);

/* (SD RW-gate wrapper removed — see do_update above: staging is on internal
 * littlefs now, so a missing archive card cannot fail a serial push.) */
static void do_update_local(const script_req_t *r)
{
    do_update_local_impl(r);
}

static void do_update_local_impl(const script_req_t *r)
{
    ESP_LOGW(TAG, "script_update(local) id=%s", r->id[0] ? r->id : "(none)");
    char detail[192] = "";

    if (s_cfg.workload_suspend == NULL || s_cfg.workload_resume == NULL) {
        ESP_LOGE(TAG, "local variant needs workload hooks, not configured");
        report_script("failed", r->id, "local variant unavailable");
        return;
    }

    /* Stop the runner: its source file is about to be swapped. Deliberately NO
     * comms_suspend, unlike the URL worker: there is no HTTPS download here that
     * needs the TLS heap, and keeping MQTT up also skips the reconnect wait on
     * the way out. This path is driven by an operator on the local console. */
    s_cfg.workload_suspend();

    bool   applied = false;
    size_t n = 0;
    char   got[65] = "";

    long staged = file_size(SCHEDULE_PATH_NEW);
    if (staged <= 0) {
        snprintf(detail, sizeof detail, "no staged schedule (schedule begin/put first)");
        ESP_LOGE(TAG, "%s", detail);
    } else if (sha256_file(SCHEDULE_PATH_NEW, got) != ESP_OK) {
        snprintf(detail, sizeof detail, "cannot hash staged script");
        ESP_LOGE(TAG, "%s", detail);
    } else {
        n = (size_t)staged;
        applied = verify_and_swap_staged(r, got, n, detail, sizeof detail);
    }

    if (!applied) {
        report_script("failed", r->id, detail);
        s_cfg.workload_resume();   /* restart the old script */
        return;
    }

    if (r->reboot) {
        ESP_LOGW(TAG, "schedule.yaml replaced from serial push (%u bytes); previous kept as %s, rebooting to run it",
                 (unsigned)n, SCHEDULE_PATH_BAK);
        snprintf(detail, sizeof detail, "%u bytes; rebooting", (unsigned)n);
        report_script_installed("applied", r->id, detail);
        vTaskDelay(pdMS_TO_TICKS(SCRIPT_REBOOT_DELAY_MS));
        esp_restart();                                       /* no return */
    }

    s_cfg.workload_resume();
    ESP_LOGW(TAG, "schedule.yaml replaced from serial push (%u bytes) + runner restarted; previous kept as %s",
             (unsigned)n, SCHEDULE_PATH_BAK);
    snprintf(detail, sizeof detail, "%u bytes", (unsigned)n);
    report_script("applied", r->id, detail);
}

/* ── shared maintenance worker dispatch (fix #3) ──────────────────────────── */

/* Run one queued script/exec op in the shared maintenance worker. Owns and frees
 * `arg` (a heap script_req_t) AND its ->text. Previously this ran on a per-op
 * lazy task with a 10 KB stack, whose xTaskCreate failed (ESP_ERR_NO_MEM) on the
 * fragmented field heap — so a remote schedule push (the primary recovery path)
 * could not launch. It now runs on the single resident maintenance worker. */
static void script_run(void *arg)
{
    script_req_t *r = arg;
    if (request_already_active(r)) {
        ESP_LOGI(TAG, "script_update id=%s already active — reporting identity",
                 r->id);
        report_script("applied", r->id, "already applied; checksum verified");
        free(r->text);
        free(r);
        return;
    }
    if (already_applied(r->id)) {
        ESP_LOGW(TAG, "script_update id=%s latch matched but active checksum drifted — reapplying",
                 r->id);
    }
    /* Global maintenance gate: refuse to overlap another update type. Redundant
     * under the single shared worker (ops are already serialized), kept as
     * belt-and-suspenders. */
    if (s_cfg.maintenance_begin != NULL && !s_cfg.maintenance_begin()) {
        ESP_LOGW(TAG, "another maintenance op in progress — script op=%u id=%s dropped",
                 r->op, r->id[0] ? r->id : "(none)");
        report_script("busy", r->id, "another maintenance op is in progress");
        free(r->text);
        free(r);
        return;
    }
    if (r->op == OP_UPDATE_URL) do_update_url(r);
    else if (r->op == OP_UPDATE_LOCAL) do_update_local(r);
    else                            do_update(r);
    if (s_cfg.maintenance_end != NULL) s_cfg.maintenance_end();
    free(r->text);
    free(r);
}

/* Hand a request to the shared maintenance worker. Takes ownership of r->text on
 * success (the worker frees it); on failure the caller (request_common) frees it.
 * The heap copy is small (~350 B), so it allocates even on a fragmented heap. */
static esp_err_t enqueue(script_req_t *r)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    script_req_t *copy = malloc(sizeof *copy);
    if (copy == NULL) return ESP_ERR_NO_MEM;   /* caller frees r->text */
    *copy = *r;                                /* copy->text aliases r->text */
    if (s_cfg.submit == NULL || !s_cfg.submit(script_run, copy)) {
        free(copy);                            /* NOT copy->text — caller still owns it */
        return ESP_ERR_NO_MEM;                 /* worker queue full — op already queued/in-flight */
    }
    return ESP_OK;                             /* worker now owns copy + copy->text */
}

/* ── public API ───────────────────────────────────────────────────────────── */

esp_err_t script_update_init(const script_update_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    s_cfg   = *cfg;
    s_ready = true;   /* ops dispatch to the shared maintenance worker via s_cfg.submit */
    ESP_LOGI(TAG, "schedule update module ready (shared maintenance worker)");
    return ESP_OK;
}

static esp_err_t request_common(uint8_t op, const char *text,
                                const char *checksum, const char *id, bool reboot,
                                bool fleet_jitter,
                                const char *script_version, const char *built_against_fw)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (text == NULL || text[0] == '\0') return ESP_ERR_INVALID_ARG;

    script_req_t r;
    memset(&r, 0, sizeof r);
    r.op = op;
    r.reboot = reboot;
    r.fleet_jitter = fleet_jitter;
    if (id != NULL) strncpy(r.id, id, sizeof r.id - 1);
    if (checksum != NULL) strncpy(r.checksum, checksum, sizeof r.checksum - 1);
    if (script_version != NULL) {
        strncpy(r.script_version, script_version, sizeof r.script_version - 1);
    }
    if (built_against_fw != NULL) {
        strncpy(r.built_against_fw, built_against_fw, sizeof r.built_against_fw - 1);
    }
    r.text = strdup(text);
    if (r.text == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = enqueue(&r);
    if (err != ESP_OK) free(r.text);
    return err;
}

/* A reboot with no id can't be deduped (identity_set/already_applied are no-ops for
 * an empty id), so a RETAINED reboot=true command would re-apply + reboot on every
 * reconnect — a boot loop. Require an id to reboot. */
static bool reboot_needs_id(const char *id, bool reboot, const char *what)
{
    if (reboot && (id == NULL || id[0] == '\0')) {
        ESP_LOGW(TAG, "%s reboot=true requires an id — rejecting (retained-loop guard)", what);
        report_script("failed", id, "reboot requires an id");
        return true;
    }
    return false;
}

esp_err_t script_update_request(const char *script, const char *checksum, const char *id,
                                bool reboot, const char *script_version,
                                const char *built_against_fw)
{
    if (reboot_needs_id(id, reboot, "script_update")) return ESP_ERR_INVALID_ARG;
    return request_common(OP_UPDATE, script, checksum, id, reboot, false,
                          script_version, built_against_fw);
}

esp_err_t script_update_url_request(const char *url, const char *checksum, const char *id,
                                    bool reboot, const char *script_version,
                                    const char *built_against_fw)
{
    if (reboot_needs_id(id, reboot, "script_update(url)")) return ESP_ERR_INVALID_ARG;
    return request_common(OP_UPDATE_URL, url, checksum, id, reboot, true,
                          script_version, built_against_fw);   /* text holds the URL */
}

esp_err_t script_update_url_request_immediate(const char *url, const char *checksum,
                                              const char *id, bool reboot,
                                              const char *script_version,
                                              const char *built_against_fw)
{
    if (reboot_needs_id(id, reboot, "script_update(url immediate)")) {
        return ESP_ERR_INVALID_ARG;
    }
    return request_common(OP_UPDATE_URL, url, checksum, id, reboot, false,
                          script_version, built_against_fw);
}

esp_err_t script_update_local_request(const char *checksum, const char *id,
                                      bool reboot, const char *script_version,
                                      const char *built_against_fw)
{
    if (reboot_needs_id(id, reboot, "script_update(local)")) return ESP_ERR_INVALID_ARG;
    /* request_common rejects an empty text, and this op carries no URL or script
     * body: the bytes are already staged on littlefs. Pass the staging path so
     * the request is self-describing in logs. */
    return request_common(OP_UPDATE_LOCAL, SCRIPT_UPDATE_STAGING_PATH, checksum, id,
                          reboot, false, script_version, built_against_fw);
}
