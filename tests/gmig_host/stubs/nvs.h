#ifndef GMIG_HOST_NVS_H
#define GMIG_HOST_NVS_H
/* G-MIG host NVS model (tests/gmig_host/nvs_model.c).
 *
 * Shared by BOTH firmware builds (v1.11.0 and the candidate) so the two
 * executables hand NVS state to each other exactly as two firmware images on
 * one device do. Semantics modelled on ESP-IDF 5.x nvs_flash (the API subset
 * any event_log revision uses, plus i32/str/erase for completeness):
 *   - entries are (namespace, key) -> (type, value); a get with the wrong type
 *     is ESP_ERR_NVS_TYPE_MISMATCH and a set that would change an existing
 *     key's type is REFUSED with the same code and logged as "type_conflict"
 *     (the harness asserts none happen: real NVS behaviour there is not
 *     something this proof may silently assume);
 *   - nvs_set_* is durable when it returns (IDF writes the entry to flash
 *     immediately; nvs_commit is a no-op kept for API compatibility): the
 *     model rewrites ./nvs.db atomically on every successful set/erase;
 *   - nvs_open(READONLY) of a namespace that was never created is
 *     ESP_ERR_NVS_NOT_FOUND; READWRITE creates it; writes on a READONLY
 *     handle are ESP_ERR_NVS_READ_ONLY; keys/namespaces are <= 15 chars;
 *   - a failed get never writes the caller's output (firmware defaults rely
 *     on that);
 *   - get_blob/get_str with a NULL buffer report the stored length; a short
 *     buffer is ESP_ERR_NVS_INVALID_LENGTH.
 * NOT modelled: page layout, wear, erase-block tearing, the 126-page entry
 * budget, multi-page blob chunking, encryption. */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifndef ESP_ERR_NVS_BASE
#define ESP_ERR_NVS_BASE 0x1100
#endif
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND        (ESP_ERR_NVS_BASE + 0x02)
#endif
#define ESP_ERR_NVS_TYPE_MISMATCH    (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY        (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_NAME     (ESP_ERR_NVS_BASE + 0x06)
#define ESP_ERR_NVS_INVALID_HANDLE   (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_KEY_TOO_LONG     (ESP_ERR_NVS_BASE + 0x09)
#ifndef ESP_ERR_NVS_INVALID_LENGTH
#define ESP_ERR_NVS_INVALID_LENGTH   (ESP_ERR_NVS_BASE + 0x0c)
#endif
#define ESP_ERR_NVS_VALUE_TOO_LONG   (ESP_ERR_NVS_BASE + 0x0e)

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *out);
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v);
esp_err_t nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *out);
esp_err_t nvs_set_u64(nvs_handle_t h, const char *key, uint64_t v);
esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *out);
esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);

/* Harness introspection: number of refused type-changing sets so far. */
unsigned gmig_nvs_type_conflicts(void);
#endif
