#include "sensor_inventory.h"
#include "ambit_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define UART_SENSOR_NUM_CHANNELS 4
#define ESP_OK 0
#define ESP_FAIL -1
#define AMBIT_INFO_FW 2
/* Only the hardware port result is fake; all CLI control flow is production. */
typedef int esp_err_t;
typedef struct {int status;} cmd_result_t;
typedef struct {bool connected;int chip;uint8_t mac[6];} ambit_flash_probe_result_t;
static unsigned wires, refresh_wires;
static bool fail_wire;
static int64_t clock_us;
static int64_t now(void) {return clock_us;}
static void lock(void) {}
static void unlock(void) {}
static void yield_ms(uint32_t ms) {clock_us+=(int64_t)ms*1000;}
static void check_veto(void)
{ assert(sensor_inventory_wait_ms()==UINT32_MAX);sensor_inventory_refresh();assert(!refresh_wires);++wires; }
static sensor_read_result_t inspect(uint8_t ch,uint8_t type,void *out,size_t n,size_t *got,uint32_t timeout,void (*started)(uint8_t))
{(void)ch;(void)type;(void)out;(void)n;(void)got;(void)timeout;(void)started;++refresh_wires;return SENSOR_READ_FAILED;}
static const char *esp_err_to_name(int e) {(void)e;return "host_error";}
static cmd_result_t cmd_uart_ping(uint8_t ch,bool *connected)
{(void)ch;check_veto();*connected=!fail_wire;return (cmd_result_t){0};}
static cmd_result_t cmd_ambit_get_info(uint8_t ch,uint8_t type,uint8_t *out,size_t n,size_t *got)
{(void)ch;assert(type==2 && n>=sizeof(ambit_fw_info_t));check_veto();memset(out,0,sizeof(ambit_fw_info_t));*got=sizeof(ambit_fw_info_t);return (cmd_result_t){0};}
static int uart_sensors_flash_session_begin(uint8_t ch,uint32_t timeout)
{(void)ch;(void)timeout;sensor_inventory_veto_begin();check_veto();if(fail_wire){sensor_inventory_veto_end();return ESP_FAIL;}return ESP_OK;}
static void uart_sensors_flash_session_end(uint8_t ch) {(void)ch;check_veto();sensor_inventory_veto_end();}
static int uart_sensors_enter_download(uint8_t ch) {(void)ch;check_veto();return ESP_OK;}
static int uart_sensors_run_app(uint8_t ch) {(void)ch;check_veto();return ESP_OK;}
static int ambit_flash_probe(uint8_t ch,ambit_flash_probe_result_t *out)
{(void)ch;check_veto();memset(out,0,sizeof *out);out->connected=!fail_wire;return fail_wire ? ESP_FAIL : ESP_OK;}
#include "cli-veto-production.inc"
int main(void)
{
    const sensor_inventory_ports_t ports={.now_us=now,.lock=lock,.unlock=unlock,.yield_ms=yield_ms,.inspect=inspect};
    sensor_inventory_init(&ports,123);
    char *all[]={"ambit_probe","all"}, *run[]={"ambit_dl","0","run"}, *dl[]={"ambit_dl","0","enter"};
    for(unsigned i=0;i<2;++i) {
        fail_wire=i;
        assert(cli_cmd_ambit_versions(0,NULL)==0);
        assert(sensor_inventory_wait_ms()!=UINT32_MAX);
        assert(cli_cmd_ambit_probe(2,all)==(fail_wire ? 1 : 0));
        assert(sensor_inventory_wait_ms()!=UINT32_MAX);
        assert(cli_cmd_ambit_dl(3,run)==(fail_wire ? 1 : 0));
        assert(cli_cmd_ambit_dl(3,dl)==(fail_wire ? 1 : 0));
        assert(sensor_inventory_wait_ms()!=UINT32_MAX);
    }
    assert(wires && !refresh_wires);puts("SENSOR_CLI_VETO_OK versions=1 probe_all=1 download=1 error_release=1");
}
