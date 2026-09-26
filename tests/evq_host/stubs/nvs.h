#ifndef EVQ_HOST_NVS_H
#define EVQ_HOST_NVS_H
/* Host NVS: a key/value map persisted atomically (tmp + rename) on
 * nvs_commit into ./nvs.txt. Sets not yet committed are lost on a simulated
 * power loss — exactly what the cursor-tear tests need. */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *out);
esp_err_t nvs_set_u64(nvs_handle_t h, const char *key, uint64_t v);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
#endif
