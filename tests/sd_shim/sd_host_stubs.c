/* Host stand-ins shared by the SD-writer harnesses: the sd_card io gate / loss
 * latch (driven by the test) and the sd_diag device API over the REAL pure core
 * (components/sd_card/sd_diag_core.c). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sd_card.h"
#include "sd_diag.h"

int g_sd_refs;
int g_sd_lost;
int g_sd_mounted = 1;
int g_sd_teardown;
unsigned g_sd_io_errors, g_sd_io_oks;
unsigned g_sd_begin_refused;

bool sdcard_io_begin(void)
{
    if (g_sd_teardown || g_sd_lost || !g_sd_mounted) { g_sd_begin_refused++; return false; }
    g_sd_refs++;
    return true;
}

void sdcard_io_end(void)
{
    if (g_sd_refs <= 0) { fprintf(stderr, "REF IMBALANCE\n"); g_sd_refs = -1000; return; }
    g_sd_refs--;
}

bool sdcard_io_lost(void) { return g_sd_lost != 0; }
bool sdcard_is_mounted(void) { return g_sd_mounted != 0; }
void sdcard_report_io_error(void) { g_sd_io_errors++; }
void sdcard_report_io_ok(void) { g_sd_io_oks++; }
esp_err_t sdcard_mount(void) { return g_sd_mounted ? ESP_OK : ESP_FAIL; }

static sd_diag_block_t s_diag;
static bool s_diag_init;

static void diag_init(void)
{
    if (s_diag_init) return;
    s_diag_init = true;
    sd_diag_core_boot(&s_diag, false);
    sd_diag_core_merge_floor(&s_diag, NULL);
}

void sd_diag_fault(sd_diag_writer_t w, sd_diag_op_t op, int err) { diag_init(); sd_diag_core_fault(&s_diag, w, op, err, 1); }
void sd_diag_refusal(sd_diag_refusal_t r, int64_t id, int err, uint8_t blocked, uint8_t sd_state)
{
    diag_init();
    sd_diag_core_refusal(&s_diag, r, id, err, blocked, sd_state, 0, 1);
}
void sd_diag_get(sd_diag_block_t *out) { diag_init(); *out = s_diag; }
void sd_diag_persist(bool force) { (void)force; }
