#ifndef AMBIT_HTTP_STUB_H
#define AMBIT_HTTP_STUB_H
/* esp_http_client stand-in: serves the driver's deterministic image bytes so the
 * baseline download function can run verbatim on the host. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/task.h"
typedef struct esp_http_client *esp_http_client_handle_t;
typedef struct {
    const char *url;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int timeout_ms;
    bool keep_alive_enable;
    int buffer_size;
    int buffer_size_tx;
} esp_http_client_config_t;
esp_err_t esp_crt_bundle_attach(void *conf);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len);
int64_t   esp_http_client_fetch_headers(esp_http_client_handle_t c);
int       esp_http_client_get_status_code(esp_http_client_handle_t c);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c);
int       esp_http_client_read(esp_http_client_handle_t c, char *buf, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
#endif
