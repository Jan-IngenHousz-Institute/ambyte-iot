#include "http_stub.h"
#include <stddef.h>

extern int64_t g_src_clen;
int src_read(void *ctx, uint8_t *buf, size_t cap);
static int s_dummy;

esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    (void)cfg;
    return (esp_http_client_handle_t)(void *)&s_dummy;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int w) { (void)c; (void)w; return ESP_OK; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return g_src_clen; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return 200; }
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len) { (void)c; return src_read(NULL, (uint8_t *)buf, (size_t)len); }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
