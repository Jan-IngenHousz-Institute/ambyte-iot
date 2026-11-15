/* The destructive primitives themselves under brownout protection (review
 * round 4 of PR #103). tests/test_power_protect.py extracts, verbatim:
 *   components/esp_serial_flasher/src/esp_loader.c   init_flash_params,
 *       esp_loader_flash_start, esp_loader_flash_start_admitted;
 *   components/uart_sensors/uart_sensors.c   do_query_admitted, do_query;
 * into primitives-production.inc. Fakes sit at the wire: the SPI attach, the
 * flash-ID read, the SPI-parameter command, loader_flash_begin_cmd (the erase),
 * the channel lock, the wake handshake and the command send. Protection
 * switches on INSIDE each blocking step; the admission must still stop the
 * erase / the OTA_BEGIN send, with nothing blocking after it.
 *
 * usage: primitives <scenario>   (exit 0 = PASS) */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *sc;
static bool pp_on;
static bool is(const char *s) { return strcmp(sc, s) == 0; }
static void flip_if(const char *s) { if (is(s)) pp_on = true; }
static bool admit(void) { return !pp_on; }
static bool admit_ctx(void *ctx) { assert(ctx == (void *)&pp_on); return !pp_on; }

/* ── esp_loader.c seams ─────────────────────────────────────────────────── */
typedef int esp_loader_error_t;
#define ESP_LOADER_SUCCESS 0
#define ESP_LOADER_ERROR_UNSUPPORTED_FUNC 1
#define ESP_LOADER_ERROR_INVALID_PARAM 2
#define ESP_LOADER_ERROR_IMAGE_SIZE 3
#define ESP_LOADER_PROTOCOL_UART 0
#define ESP_LOADER_PROTOCOL_SPI 1
#define ESP_LOADER_PROTOCOL_SDIO 2
#define ESP8266_CHIP 0
#define ESP32C3_CHIP 3
#define MAX_ROM_FLASH_SIZE (16u * 1024 * 1024)
#define DEFAULT_FLASH_SIZE (2u * 1024 * 1024)
#define DEFAULT_TIMEOUT 1000
#define ERASE_FLASH_TIMEOUT_PER_MB 10000
#define RETURN_ON_ERROR(x) do { esp_loader_error_t rc_ = (x); if (rc_ != ESP_LOADER_SUCCESS) return rc_; } while (0)
#define LOADER_LOGI(loader, ...) ((void)(loader))
#define LOADER_LOGW(loader, ...) ((void)(loader))
typedef struct port port_t;
typedef struct { void (*start_timer)(port_t *, uint32_t); } port_ops_t;
struct port { const port_ops_t *ops; };
typedef struct {
    int _protocol_type, _target;
    bool _stub_running;
    uint32_t _target_flash_size;
    port_t *_port;
} esp_loader_t;
typedef struct {
    uint32_t offset, image_size, block_size;
    bool skip_verify;
    struct { unsigned _sequence_number; } _state;
} esp_loader_flash_cfg_t;
static unsigned erases, spi_attaches, id_reads, spi_params;
static esp_loader_error_t loader_ensure_spi_attached(esp_loader_t *l) { (void)l; ++spi_attaches; flip_if("rom_flip_during_spi_attach"); return 0; }
static esp_loader_error_t esp_loader_flash_detect_size(esp_loader_t *l, uint32_t *n) { (void)l; ++id_reads; flip_if("rom_flip_during_detect"); *n = 4u * 1024 * 1024; return 0; }
static esp_loader_error_t loader_spi_parameters(esp_loader_t *l, uint32_t n) { (void)l; (void)n; ++spi_params; flip_if("rom_flip_during_spi_params"); return 0; }
static void init_md5(esp_loader_flash_cfg_t *c) { (void)c; }
static bool encryption_in_begin_flash_cmd(int t) { (void)t; return false; }
static uint32_t calc_erase_size(int t, bool s, uint32_t o, uint32_t n) { (void)t; (void)s; (void)o; return n; }
static uint32_t timeout_per_mb(uint32_t n, uint32_t m) { (void)n; return m; }
static esp_loader_error_t loader_flash_begin_cmd(esp_loader_t *l, unsigned *s, uint32_t o, uint32_t e, uint32_t z, uint32_t b, bool enc)
{ (void)l; (void)s; (void)o; (void)e; (void)z; (void)b; (void)enc; ++erases; return 0; }
static void start_timer(port_t *p, uint32_t ms) { (void)p; (void)ms; }

/* ── uart_sensors.c seams ───────────────────────────────────────────────── */
typedef int esp_err_t;
typedef int uart_port_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_ALLOWED 0x10D
#define TAG "uart"
#define UART_SENSOR_NUM_CHANNELS 4
#define pdMS_TO_TICKS(x) (x)
#define UART_QUERY_ACK_ONLY ((size_t)-1)
#define AMBIT_CMD_DONE 161
#define AMBIT_CMD_OTA_BEGIN 25
typedef enum { UART_SENSOR_DISCONNECTED = 0, UART_SENSOR_CONNECTED = 1, UART_SENSOR_UNKNOWN = 2 } uart_sensor_state_t;
typedef struct { esp_err_t status; uint8_t *raw; size_t raw_len; } uart_sensor_response_t;
__attribute__((format(printf, 2, 3))) static void host_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)
static bool s_inited = true;
static struct { int uart_num; uart_sensor_state_t state; bool ping_ok; int64_t ping_ts; } s_ch[UART_SENSOR_NUM_CHANNELS];
static int locks;
static unsigned sends, begin_sends, wakes;
static int64_t now_us(void) { return 1; }
static esp_err_t channel_acquire(uint8_t ch, uint32_t ms) { (void)ch; (void)ms; assert(locks == 0); ++locks; flip_if("uart_flip_during_channel_wait"); return ESP_OK; }
static void channel_release(uint8_t ch) { (void)ch; assert(locks == 1); --locks; }
static esp_err_t ambit_wake(uart_port_t p, int64_t d) { (void)p; (void)d; ++wakes; flip_if("uart_flip_during_wake"); return ESP_OK; }
static esp_err_t ambit_send_cmd(uart_port_t p, const uint8_t cmd[8], const uint8_t *x, size_t n)
{ (void)p; (void)x; (void)n; ++sends; if (cmd[0] == AMBIT_CMD_OTA_BEGIN) ++begin_sends; return ESP_OK; }
static esp_err_t uart_scan_byte(uart_port_t p, uint8_t b, int64_t d) { (void)p; (void)b; (void)d; return ESP_OK; }
static esp_err_t receive_raw_response(uart_port_t p, uart_sensor_response_t *r, size_t n, int64_t d) { (void)p; (void)n; (void)d; r->status = ESP_OK; return ESP_OK; }
static esp_err_t receive_fsm_response(uart_port_t p, uart_sensor_response_t *r, int64_t d) { (void)p; (void)d; r->status = ESP_OK; return ESP_OK; }

#include "primitives-production.inc"

static esp_loader_error_t rom_flash_start(bool gated, bool *refused)
{
    static const port_ops_t ops = { start_timer };
    static port_t port = { &ops };
    /* As after esp_loader_connect_with_stub: flash size not yet known. */
    esp_loader_t l = { ._protocol_type = ESP_LOADER_PROTOCOL_UART, ._target = ESP32C3_CHIP,
                       ._stub_running = true, ._target_flash_size = 0, ._port = &port };
    esp_loader_flash_cfg_t cfg = { .offset = 0, .image_size = 4096, .block_size = 1024 };
    assert(!pp_on);   /* the caller's earlier admissions passed */
    return gated ? esp_loader_flash_start_admitted(&l, &cfg, admit_ctx, (void *)&pp_on, refused)
                 : esp_loader_flash_start(&l, &cfg);
}

static esp_err_t uart_begin(bool gated)
{
    const uint8_t cmd[8] = { AMBIT_CMD_OTA_BEGIN, 0, 16, 0, 0, 0, 0, 0 };
    uart_sensor_response_t r;
    s_ch[0].state = UART_SENSOR_CONNECTED;
    assert(!pp_on);
    return gated ? do_query_admitted(0, cmd, NULL, 0, 1, &r, 15000, admit)
                 : do_query(0, cmd, NULL, 0, 1, &r, 15000);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    sc = argv[1];
    if (strncmp(sc, "rom_flip_", 9) == 0) {
        bool refused = false;
        const esp_loader_error_t rc = rom_flash_start(true, &refused);
        /* The flash setup really ran (attach, ID read, SPI params) ... */
        assert(spi_attaches >= 1 && id_reads == 1 && spi_params == 1 && pp_on);
        /* ... and the admission after it stopped the erase. */
        assert(rc == ESP_LOADER_SUCCESS && refused && erases == 0);
    } else if (is("rom_admitted")) {
        bool refused = true;
        assert(rom_flash_start(true, &refused) == ESP_LOADER_SUCCESS && !refused && erases == 1);
    } else if (is("rom_ungated_wrapper")) {
        /* esp_loader_flash_start (the USB console path) is upstream behaviour:
         * protection during the setup does not stop it. */
        sc = "rom_flip_during_spi_params";
        assert(rom_flash_start(false, NULL) == ESP_LOADER_SUCCESS && erases == 1 && pp_on);
        sc = "rom_ungated_wrapper";
    } else if (strncmp(sc, "uart_flip_", 10) == 0) {
        const esp_err_t rc = uart_begin(true);
        assert(rc == ESP_ERR_NOT_ALLOWED && begin_sends == 0 && sends == 0 && wakes == (is("uart_flip_during_wake") || is("uart_flip_during_channel_wait") ? 1u : 0u));
        assert(locks == 0);                                   /* channel released normally */
        assert(s_ch[0].state == UART_SENSOR_CONNECTED);       /* a refusal is not a link fault */
    } else if (is("uart_admitted")) {
        assert(uart_begin(true) == ESP_OK && begin_sends == 1 && locks == 0);
    } else if (is("uart_ungated_query")) {
        /* do_query (DATA / END / every other command) is unchanged. */
        sc = "uart_flip_during_wake";
        assert(uart_begin(false) == ESP_OK && begin_sends == 1 && pp_on && locks == 0);
        sc = "uart_ungated_query";
    } else {
        fprintf(stderr, "unknown scenario %s\n", sc);
        return 2;
    }
    printf("PASS %s\n", sc);
    return 0;
}
