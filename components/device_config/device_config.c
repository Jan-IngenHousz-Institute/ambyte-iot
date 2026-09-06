#include "device_config.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "timezone.h"

#define TAG          "device_cfg"
#define NVS_NS       "device_cfg"

/* NVS key names — max 15 chars (NVS_KEY_NAME_MAX_SIZE = 16 incl. null) */
#define KEY_MQTT_URI        "mqtt_uri"
#define KEY_MQTT_CLIENT_ID  "mqtt_client_id"
#define KEY_MQTT_TOPIC_ROOT "mqtt_topic_root"
#define KEY_COMMAND_TOPIC   "cmd_topic"
#define KEY_STATUS_TOPIC    "status_topic"
#define KEY_DEVICE_ID       "device_id"
#define KEY_PROTOCOL_ID     "protocol_id"
#define KEY_DEVICE_NAME     "device_name"
#define KEY_DEVICE_VERSION  "device_ver"
#define KEY_DEVICE_FIRMWARE "device_firm"
#define KEY_FIRMWARE_VER    "firmware_ver"
#define KEY_TIMEZONE        "timezone"
#define KEY_FLASH_TIME      "flash_time"
#define KEY_HEARTBEAT_S     "heartbeat_s"
#define KEY_PUBLISH_GZIP    "publish_gzip"
#define KEY_LAT             "lat"
#define KEY_LON             "lon"
#define KEY_DEPLOYMENT      "deployment"
#define KEY_SITE_STATE      "site_state"

/* Atomic site-state blob, shared byte-for-byte with tools/site_state_blob.py.
 * Do not cast this to a C struct: explicit offsets avoid ABI padding and make
 * the provisioning image independent of the Python host's architecture.
 *
 *   0..3   "AMST" magic
 *   4      version (1)
 *   5      presence flags: lat/lon/deployment
 *   6      deployment byte length (0..63)
 *   7      reserved zero
 *   8..15  latitude IEEE-754 binary64, little-endian
 *   16..23 longitude IEEE-754 binary64, little-endian
 *   24..87 deployment bytes, NUL terminator, zero padding
 *
 * NVS replaces one key transactionally at nvs_commit(), so a power cut sees
 * either the complete old blob or the complete new blob. The legacy three
 * keys remain read-only fallback for devices provisioned before version 1. */
#define SITE_STATE_BLOB_SIZE        88U
#define SITE_STATE_VERSION          1U
#define SITE_STATE_FLAG_LAT         (1U << 0)
#define SITE_STATE_FLAG_LON         (1U << 1)
#define SITE_STATE_FLAG_DEPLOYMENT  (1U << 2)
#define SITE_STATE_KNOWN_FLAGS      (SITE_STATE_FLAG_LAT | SITE_STATE_FLAG_LON | \
                                     SITE_STATE_FLAG_DEPLOYMENT)
#define SITE_STATE_DEPLOYMENT_CAP   63U

_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "site_state requires IEEE-754 binary64 double");

typedef struct {
    bool has_lat;
    bool has_lon;
    bool has_deployment;
    double lat;
    double lon;
    char deployment[SITE_STATE_DEPLOYMENT_CAP + 1];
} site_state_t;

static nvs_handle_t s_handle    = 0;
static bool         s_initialized = false;

/* RAM cache of KEY_PUBLISH_GZIP so the per-publish check in the MQTT drain
 * path never touches NVS. Loaded once in device_config_init, updated by the
 * setter. Absent/invalid values read as disabled — plain JSON is the safe
 * default until the OpenJII ingest confirms gzip support for this producer. */
static bool         s_publish_gzip = false;

esp_err_t device_config_init(void)
{
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;

    {
        char gz[8];
        size_t gz_len = sizeof(gz);
        s_publish_gzip = nvs_get_str(s_handle, KEY_PUBLISH_GZIP, gz, &gz_len) == ESP_OK
                         && strcmp(gz, "1") == 0;
    }

    /* One-time OTA-safe migration for early provisioning images that used AMT
     * as shorthand for Amsterdam. Do not rewrite unknown values: retain them in
     * NVS for diagnosis, while device_config_get_timezone() below fails closed
     * so they can never enter an MQTT envelope. */
    char stored[64];
    size_t stored_len = sizeof(stored);
    err = nvs_get_str(s_handle, KEY_TIMEZONE, stored, &stored_len);
    if (err == ESP_OK) {
        char canonical[48];
        esp_err_t tzerr = timezone_canonicalize(stored, canonical, sizeof(canonical));
        if (tzerr == ESP_OK && strcmp(stored, canonical) != 0) {
            tzerr = nvs_set_str(s_handle, KEY_TIMEZONE, canonical);
            if (tzerr == ESP_OK) tzerr = nvs_commit(s_handle);
            if (tzerr == ESP_OK) {
                ESP_LOGW(TAG, "timezone migrated '%s' -> '%s'", stored, canonical);
            } else {
                ESP_LOGE(TAG, "timezone migration failed: %s", esp_err_to_name(tzerr));
                return tzerr;
            }
        } else if (tzerr != ESP_OK) {
            ESP_LOGE(TAG, "invalid stored timezone '%s' — omitted from telemetry", stored);
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "timezone migration read failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "device_config initialised");
    return ESP_OK;
}

static esp_err_t cfg_get(const char *key, char *buf, size_t len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    size_t out_len = len;
    return nvs_get_str(s_handle, key, buf, &out_len);
}

static esp_err_t cfg_set(const char *key, const char *val)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t err = nvs_set_str(s_handle, key, val);
    if (err != ESP_OK) return err;
    return nvs_commit(s_handle);
}

esp_err_t device_config_get_mqtt_uri(char *buf, size_t len)
{
    return cfg_get(KEY_MQTT_URI, buf, len);
}

esp_err_t device_config_get_mqtt_client_id(char *buf, size_t len)
{
    return cfg_get(KEY_MQTT_CLIENT_ID, buf, len);
}

esp_err_t device_config_get_mqtt_topic_root(char *buf, size_t len)
{
    return cfg_get(KEY_MQTT_TOPIC_ROOT, buf, len);
}

esp_err_t device_config_set_mqtt_uri(const char *val)
{
    return cfg_set(KEY_MQTT_URI, val);
}

esp_err_t device_config_set_mqtt_client_id(const char *val)
{
    return cfg_set(KEY_MQTT_CLIENT_ID, val);
}

esp_err_t device_config_set_mqtt_topic_root(const char *val)
{
    return cfg_set(KEY_MQTT_TOPIC_ROOT, val);
}

esp_err_t device_config_get_command_topic(char *buf, size_t len)
{
    return cfg_get(KEY_COMMAND_TOPIC, buf, len);
}

esp_err_t device_config_set_command_topic(const char *val)
{
    return cfg_set(KEY_COMMAND_TOPIC, val);
}

esp_err_t device_config_get_status_topic(char *buf, size_t len)
{
    return cfg_get(KEY_STATUS_TOPIC, buf, len);
}

esp_err_t device_config_set_status_topic(const char *val)
{
    return cfg_set(KEY_STATUS_TOPIC, val);
}

esp_err_t device_config_get_timezone(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    char stored[64];
    esp_err_t err = cfg_get(KEY_TIMEZONE, stored, sizeof(stored));
    if (err != ESP_OK) return err;
    return timezone_canonicalize(stored, buf, len);
}

esp_err_t device_config_get_flash_time(uint32_t *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    return nvs_get_u32(s_handle, KEY_FLASH_TIME, out);
}

esp_err_t device_config_get_heartbeat_s(uint32_t *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    return nvs_get_u32(s_handle, KEY_HEARTBEAT_S, out);
}

static esp_err_t cfg_get_double(const char *key, double *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    size_t len = sizeof(*out);
    return nvs_get_blob(s_handle, key, out, &len);
}

static uint64_t site_get_u64le(const uint8_t *src)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)src[i] << (i * 8);
    return value;
}

static void site_put_u64le(uint8_t *dst, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) dst[i] = (uint8_t)(value >> (i * 8));
}

static double site_get_double(const uint8_t *src)
{
    uint64_t bits = site_get_u64le(src);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void site_put_double(uint8_t *dst, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    site_put_u64le(dst, bits);
}

static bool bytes_are_zero(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0) return false;
    }
    return true;
}

static bool bytes_are_utf8(const uint8_t *buf, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t c = buf[i++];
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) {
            if (i >= len || (buf[i++] & 0xc0) != 0x80) return false;
            continue;
        }
        if (c >= 0xe0 && c <= 0xef) {
            if (i + 1 >= len) return false;
            uint8_t c1 = buf[i++], c2 = buf[i++];
            if ((c2 & 0xc0) != 0x80 ||
                (c == 0xe0 ? c1 < 0xa0 || c1 > 0xbf
                           : c == 0xed ? c1 < 0x80 || c1 > 0x9f
                                      : (c1 & 0xc0) != 0x80)) {
                return false;
            }
            continue;
        }
        if (c >= 0xf0 && c <= 0xf4) {
            if (i + 2 >= len) return false;
            uint8_t c1 = buf[i++], c2 = buf[i++], c3 = buf[i++];
            if ((c2 & 0xc0) != 0x80 || (c3 & 0xc0) != 0x80 ||
                (c == 0xf0 ? c1 < 0x90 || c1 > 0xbf
                           : c == 0xf4 ? c1 < 0x80 || c1 > 0x8f
                                      : (c1 & 0xc0) != 0x80)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

static bool site_state_decode(const uint8_t blob[SITE_STATE_BLOB_SIZE],
                              site_state_t *out)
{
    if (memcmp(blob, "AMST", 4) != 0 || blob[4] != SITE_STATE_VERSION ||
        (blob[5] & ~SITE_STATE_KNOWN_FLAGS) != 0 || blob[7] != 0) {
        return false;
    }

    site_state_t state = {0};
    state.has_lat = (blob[5] & SITE_STATE_FLAG_LAT) != 0;
    state.has_lon = (blob[5] & SITE_STATE_FLAG_LON) != 0;
    state.has_deployment = (blob[5] & SITE_STATE_FLAG_DEPLOYMENT) != 0;

    if (state.has_lat) {
        state.lat = site_get_double(blob + 8);
        if (!isfinite(state.lat) || state.lat < -90.0 || state.lat > 90.0) return false;
    } else if (!bytes_are_zero(blob + 8, 8)) {
        return false;
    }
    if (state.has_lon) {
        state.lon = site_get_double(blob + 16);
        if (!isfinite(state.lon) || state.lon < -180.0 || state.lon > 180.0) return false;
    } else if (!bytes_are_zero(blob + 16, 8)) {
        return false;
    }

    size_t deployment_len = blob[6];
    if (state.has_deployment) {
        if (deployment_len > SITE_STATE_DEPLOYMENT_CAP ||
            memchr(blob + 24, '\0', deployment_len) != NULL ||
            !bytes_are_utf8(blob + 24, deployment_len) ||
            !bytes_are_zero(blob + 24 + deployment_len,
                            SITE_STATE_DEPLOYMENT_CAP + 1 - deployment_len)) {
            return false;
        }
        memcpy(state.deployment, blob + 24, deployment_len);
        state.deployment[deployment_len] = '\0';
    } else if (deployment_len != 0 || !bytes_are_zero(blob + 24, 64)) {
        return false;
    }

    *out = state;
    return true;
}

static void site_state_encode(const site_state_t *state,
                              uint8_t blob[SITE_STATE_BLOB_SIZE])
{
    memset(blob, 0, SITE_STATE_BLOB_SIZE);
    memcpy(blob, "AMST", 4);
    blob[4] = SITE_STATE_VERSION;
    if (state->has_lat) {
        blob[5] |= SITE_STATE_FLAG_LAT;
        site_put_double(blob + 8, state->lat);
    }
    if (state->has_lon) {
        blob[5] |= SITE_STATE_FLAG_LON;
        site_put_double(blob + 16, state->lon);
    }
    if (state->has_deployment) {
        size_t len = strlen(state->deployment);
        blob[5] |= SITE_STATE_FLAG_DEPLOYMENT;
        blob[6] = (uint8_t)len;
        memcpy(blob + 24, state->deployment, len);
        /* zero-filled byte 24+len is the required terminator */
    }
}

static esp_err_t site_state_read_blob(site_state_t *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    uint8_t blob[SITE_STATE_BLOB_SIZE];
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(s_handle, KEY_SITE_STATE, blob, &len);
    if (err != ESP_OK) return err;
    if (len != sizeof(blob) || !site_state_decode(blob, out)) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static esp_err_t site_state_read_legacy(site_state_t *out)
{
    site_state_t state = {0};
    esp_err_t first_error = ESP_ERR_NVS_NOT_FOUND;

    esp_err_t err = cfg_get_double(KEY_LAT, &state.lat);
    if (err == ESP_OK) {
        if (!isfinite(state.lat) || state.lat < -90.0 || state.lat > 90.0) {
            return ESP_ERR_INVALID_ARG;
        }
        state.has_lat = true;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        first_error = err;
    }

    err = cfg_get_double(KEY_LON, &state.lon);
    if (err == ESP_OK) {
        if (!isfinite(state.lon) || state.lon < -180.0 || state.lon > 180.0) {
            return ESP_ERR_INVALID_ARG;
        }
        state.has_lon = true;
    } else if (err != ESP_ERR_NVS_NOT_FOUND &&
               first_error == ESP_ERR_NVS_NOT_FOUND) {
        first_error = err;
    }

    err = cfg_get(KEY_DEPLOYMENT, state.deployment, sizeof(state.deployment));
    if (err == ESP_OK) {
        state.has_deployment = true;
    } else if (err != ESP_ERR_NVS_NOT_FOUND &&
               first_error == ESP_ERR_NVS_NOT_FOUND) {
        first_error = err;
    }

    if (state.has_lat || state.has_lon || state.has_deployment) {
        *out = state;
        return ESP_OK;
    }
    return first_error;
}

static esp_err_t site_state_read(site_state_t *out)
{
    esp_err_t blob_err = site_state_read_blob(out);
    if (blob_err == ESP_OK) return ESP_OK;
    if (blob_err == ESP_ERR_INVALID_STATE) return blob_err;

    /* A valid blob is authoritative, including its absent-field flags. Only an
     * absent/corrupt/unknown blob falls back to the pre-v1 keys. Reads never
     * depend on migration succeeding: migration happens only on a later write. */
    if (blob_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "site_state blob ignored (%s); trying legacy keys",
                 esp_err_to_name(blob_err));
    }
    esp_err_t legacy_err = site_state_read_legacy(out);
    if (legacy_err == ESP_OK) return ESP_OK;
    return blob_err == ESP_ERR_NVS_NOT_FOUND ? legacy_err : blob_err;
}

static esp_err_t site_state_write(const site_state_t *state)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    uint8_t blob[SITE_STATE_BLOB_SIZE];
    site_state_encode(state, blob);
    esp_err_t err = nvs_set_blob(s_handle, KEY_SITE_STATE, blob, sizeof(blob));
    if (err != ESP_OK) return err;
    return nvs_commit(s_handle);
}

static esp_err_t site_state_for_update(site_state_t *state)
{
    esp_err_t err = site_state_read(state);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(state, 0, sizeof(*state));
        return ESP_OK;
    }
    return err;
}

esp_err_t device_config_get_location(double *lat, double *lon,
                                     char *deployment, size_t deployment_len)
{
    if (lat == NULL || lon == NULL ||
        (deployment == NULL && deployment_len != 0) ||
        (deployment != NULL && deployment_len == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    site_state_t state;
    esp_err_t err = site_state_read(&state);
    if (err != ESP_OK) return err;
    if (!state.has_lat || !state.has_lon) return ESP_ERR_NVS_NOT_FOUND;
    if (deployment != NULL) {
        size_t len = state.has_deployment ? strlen(state.deployment) : 0;
        if (len + 1 > deployment_len) return ESP_ERR_NVS_INVALID_LENGTH;
        memcpy(deployment, state.has_deployment ? state.deployment : "", len + 1);
    }
    *lat = state.lat;
    *lon = state.lon;
    return ESP_OK;
}

esp_err_t device_config_get_lat(double *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    site_state_t state;
    esp_err_t err = site_state_read(&state);
    if (err != ESP_OK) return err;
    if (!state.has_lat) return ESP_ERR_NVS_NOT_FOUND;
    *out = state.lat;
    return ESP_OK;
}

esp_err_t device_config_get_lon(double *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    site_state_t state;
    esp_err_t err = site_state_read(&state);
    if (err != ESP_OK) return err;
    if (!state.has_lon) return ESP_ERR_NVS_NOT_FOUND;
    *out = state.lon;
    return ESP_OK;
}

esp_err_t device_config_get_deployment(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    site_state_t state;
    esp_err_t err = site_state_read(&state);
    if (err != ESP_OK) return err;
    if (!state.has_deployment) return ESP_ERR_NVS_NOT_FOUND;
    size_t stored_len = strlen(state.deployment) + 1;
    if (stored_len > len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(buf, state.deployment, stored_len);
    return ESP_OK;
}

esp_err_t device_config_set_lat(double val)
{
    if (!isfinite(val) || val < -90.0 || val > 90.0) return ESP_ERR_INVALID_ARG;
    site_state_t state;
    esp_err_t err = site_state_for_update(&state);
    if (err != ESP_OK) return err;
    state.lat = val;
    state.has_lat = true;
    return site_state_write(&state);
}

esp_err_t device_config_set_lon(double val)
{
    if (!isfinite(val) || val < -180.0 || val > 180.0) return ESP_ERR_INVALID_ARG;
    site_state_t state;
    esp_err_t err = site_state_for_update(&state);
    if (err != ESP_OK) return err;
    state.lon = val;
    state.has_lon = true;
    return site_state_write(&state);
}

esp_err_t device_config_set_deployment(const char *val)
{
    size_t len = val != NULL ? strlen(val) : 0;
    if (val == NULL || len > SITE_STATE_DEPLOYMENT_CAP ||
        !bytes_are_utf8((const uint8_t *)val, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    site_state_t state;
    esp_err_t err = site_state_for_update(&state);
    if (err != ESP_OK) return err;
    strcpy(state.deployment, val);
    state.has_deployment = true;
    return site_state_write(&state);
}

esp_err_t device_config_set_location(double lat, double lon,
                                     const char *deployment)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!isfinite(lat) || lat < -90.0 || lat > 90.0 ||
        !isfinite(lon) || lon < -180.0 || lon > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (deployment != NULL) {
        size_t len = strlen(deployment);
        if (len > SITE_STATE_DEPLOYMENT_CAP ||
            !bytes_are_utf8((const uint8_t *)deployment, len)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    site_state_t state = {0};
    if (deployment == NULL) {
        /* Preserve the authoritative deployment flag/value. A missing blob may
         * use legacy keys; a corrupt blob with no readable legacy tuple fails
         * closed rather than silently dropping an unknown deployment tag. */
        esp_err_t err = site_state_for_update(&state);
        if (err != ESP_OK) return err;
    } else {
        strcpy(state.deployment, deployment);
        state.has_deployment = true;
    }
    state.lat = lat;
    state.lon = lon;
    state.has_lat = true;
    state.has_lon = true;
    return site_state_write(&state);
}

esp_err_t device_config_set_timezone(const char *val)
{
    char canonical[48];
    esp_err_t err = timezone_canonicalize(val, canonical, sizeof(canonical));
    if (err != ESP_OK) return err;
    return cfg_set(KEY_TIMEZONE, canonical);
}

esp_err_t device_config_get_device_id(char *buf, size_t len)
{
    return cfg_get(KEY_DEVICE_ID, buf, len);
}

esp_err_t device_config_set_device_id(const char *val)
{
    return cfg_set(KEY_DEVICE_ID, val);
}

esp_err_t device_config_get_protocol_id(char *buf, size_t len)
{
    return cfg_get(KEY_PROTOCOL_ID, buf, len);
}

esp_err_t device_config_set_protocol_id(const char *val)
{
    return cfg_set(KEY_PROTOCOL_ID, val);
}

esp_err_t device_config_get_device_name(char *buf, size_t len)
{
    return cfg_get(KEY_DEVICE_NAME, buf, len);
}

esp_err_t device_config_set_device_name(const char *val)
{
    return cfg_set(KEY_DEVICE_NAME, val);
}

esp_err_t device_config_get_device_version(char *buf, size_t len)
{
    return cfg_get(KEY_DEVICE_VERSION, buf, len);
}

esp_err_t device_config_set_device_version(const char *val)
{
    return cfg_set(KEY_DEVICE_VERSION, val);
}

esp_err_t device_config_get_device_firmware(char *buf, size_t len)
{
    return cfg_get(KEY_DEVICE_FIRMWARE, buf, len);
}

esp_err_t device_config_set_device_firmware(const char *val)
{
    return cfg_set(KEY_DEVICE_FIRMWARE, val);
}

esp_err_t device_config_get_firmware_version(char *buf, size_t len)
{
    return cfg_get(KEY_FIRMWARE_VER, buf, len);
}

esp_err_t device_config_set_firmware_version(const char *val)
{
    return cfg_set(KEY_FIRMWARE_VER, val);
}

bool device_config_publish_gzip_enabled(void)
{
    return s_initialized && s_publish_gzip;
}

esp_err_t device_config_get_publish_gzip(char *buf, size_t len)
{
    if (buf == NULL || len < 2) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    buf[0] = s_publish_gzip ? '1' : '0';
    buf[1] = '\0';
    return ESP_OK;
}

esp_err_t device_config_set_publish_gzip(const char *val)
{
    bool enable;
    if (val != NULL && (strcmp(val, "1") == 0 || strcmp(val, "on") == 0 ||
                        strcmp(val, "true") == 0)) {
        enable = true;
    } else if (val != NULL && (strcmp(val, "0") == 0 || strcmp(val, "off") == 0 ||
                               strcmp(val, "false") == 0)) {
        enable = false;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = cfg_set(KEY_PUBLISH_GZIP, enable ? "1" : "0");
    if (err == ESP_OK) s_publish_gzip = enable;
    return err;
}
