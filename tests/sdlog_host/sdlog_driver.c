/* Scripted driver for the PRODUCTION sd_logger.c (tests/sdlog_host). Reads one
 * command per stdin line, prints JSON lines. The writer task runs in lock-step
 * (tests/sd_shim/host_rtos.c); SD file ops go through tests/sd_shim/sd_shim.c.
 * Built twice: against the working tree and against `git show <rev>:` sources
 * (-DSDLOG_BASE: the baseline has no sd_logger_acct). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_diag.h"
#include "sd_logger.h"
#include "sd_shim.h"
#if defined(CONFIG_AMBYTE_EVQ_HIL) && CONFIG_AMBYTE_EVQ_HIL
#include "evq_hil_sdl.h"   /* Sprint 2 H2/H8b trace, host variant (SLT-1, ARM-1, QUI-1) */
#endif

/* sd_shim.h renames libc calls for the PRODUCTION TU; the driver wants the real ones. */
#undef fopen
#undef fclose
#undef fwrite
#undef fread
#undef fflush
#undef fsync
#undef ftruncate
#undef fseek
#undef ftell
#undef fgetc
#undef rename
#undef remove
#undef mkdir
#undef stat
#undef opendir
#undef time

extern FILE *g_console;
extern int g_refs_at_delay_violations;
extern unsigned g_sd_begin_refused;
void host_step(int n);
TickType_t host_tick(void);

static unsigned s_calls;

static void emit_call(const char *fmt, ...)
{
    /* The exact text the log sink receives (what vsnprintf would produce with an
     * unbounded buffer) — the harness normalises it independently. */
    char big[8192];
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    vsnprintf(big, sizeof big, fmt, ap);
    printf("{\"call\":%u,\"fmt_hex\":\"", s_calls++);
    for (const char *p = fmt; *p; p++) printf("%02x", (unsigned char)*p);
    printf("\",\"text_hex\":\"");
    for (const char *p = big; *p; p++) printf("%02x", (unsigned char)*p);
    printf("\"}\n");
    fflush(stdout);
    extern void host_log_vwrite(const char *fmt, va_list ap);
    host_log_vwrite(fmt, ap2);
    va_end(ap2);
    va_end(ap);
}

static int unhex(const char *h, char *out, size_t cap)
{
    size_t n = 0;
    while (h[0] && h[1] && n + 1 < cap) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (char)v;
        h += 2;
    }
    out[n] = '\0';
    return (int)n;
}

int main(int argc, char **argv)
{
    if (argc < 2 || chdir(argv[1]) != 0) { fprintf(stderr, "usage: sdlog_driver <state-dir>\n"); return 2; }
    mkdir("sdcard", 0777);
    g_console = fopen("console.log", "a");
    shim_open_log("ops.jsonl");
    char line[20000];
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32] = "";
        sscanf(line, "%31s", cmd);
        if (strcmp(cmd, "init") == 0) {
            printf("{\"init\":%d}\n", (int)sd_logger_init());
        } else if (strcmp(cmd, "log") == 0) {
            unsigned seq = 0, len = 0;
            sscanf(line, "%*s %u %u", &seq, &len);
            char msg[4096];
            int n = snprintf(msg, sizeof msg, "SEQ=%06u|", seq);
            for (unsigned i = (unsigned)n; i < len && i < sizeof msg - 1; i++) msg[i] = (char)('a' + (i % 26));
            msg[len < sizeof msg - 1 ? (len > (unsigned)n ? len : (unsigned)n) : sizeof msg - 1] = '\0';
            emit_call("W (0) drv: %s\n", msg);
        } else if (strcmp(cmd, "logf") == 0) {
            /* logf <fmt_hex> <arg_hex>: arbitrary format with one %s argument */
            char fh[4096] = "", ah[16384] = "";
            sscanf(line, "%*s %4095s %16383s", fh, ah);
            static char f[4096], a[8192];
            unhex(fh, f, sizeof f);
            if (strcmp(ah, "-") == 0) a[0] = '\0'; else unhex(ah, a, sizeof a);
            emit_call(f, a);
        } else if (strcmp(cmd, "step") == 0) {
            int n = 1;
            sscanf(line, "%*s %d", &n);
            host_step(n);
        } else if (strcmp(cmd, "arm") == 0) {
            char op[32], mode[32], sub[128] = "";
            unsigned nth = 1, count = 1;
            int k = sscanf(line, "%*s %31s %31s %u %u %127s", op, mode, &nth, &count, sub);
            shim_mode_t m;
            if (k < 2 || shim_parse_mode(mode, &m) != 0 || shim_arm(op, k >= 5 ? sub : NULL, m, nth, count) != 0) {
                printf("{\"arm\":\"bad\"}\n");
            } else {
                printf("{\"arm\":\"ok\"}\n");
            }
        } else if (strcmp(cmd, "disarm") == 0) {
            shim_disarm();
        } else if (strcmp(cmd, "lose") == 0) {
            g_sd_lost = 1;
        } else if (strcmp(cmd, "remount") == 0) {
            g_sd_lost = 0;
            g_sd_mounted = 1;
        } else if (strcmp(cmd, "pause") == 0) {
            TickType_t t0 = host_tick();
            unsigned ops0 = shim_op_count("fwrite") + shim_op_count("fopen") + shim_op_count("fsync");
            sd_logger_pause();
            printf("{\"pause_ticks\":%u,\"ops_during\":%u}\n", (unsigned)(host_tick() - t0),
                   shim_op_count("fwrite") + shim_op_count("fopen") + shim_op_count("fsync") - ops0);
        } else if (strcmp(cmd, "resume") == 0) {
            sd_logger_resume();
        } else if (strcmp(cmd, "stop") == 0) {
            TickType_t t0 = host_tick();
            sd_logger_prepare_shutdown();
            printf("{\"stop_ticks\":%u}\n", (unsigned)(host_tick() - t0));
        } else if (strcmp(cmd, "opcount") == 0) {
            printf("{\"fopen\":%u,\"fwrite\":%u,\"fflush\":%u,\"fsync\":%u,\"fclose\":%u,\"ftruncate\":%u,"
                   "\"rename\":%u,\"remove\":%u,\"mkdir\":%u,\"stat\":%u}\n",
                   shim_op_count("fopen"), shim_op_count("fwrite"), shim_op_count("fflush"), shim_op_count("fsync"),
                   shim_op_count("fclose"), shim_op_count("ftruncate"), shim_op_count("rename"),
                   shim_op_count("remove"), shim_op_count("mkdir"), shim_op_count("stat"));
        } else if (strcmp(cmd, "acct") == 0) {
            bool active = false;
            size_t buffered = 0, dropped = 0, file_bytes = 0;
            sd_logger_stats(&active, &buffered, &dropped, &file_bytes);
            printf("{\"variant\":\"%s\",\"tick\":%u,\"io_errors\":%u,\"io_oks\":%u,\"violations\":%u,"
                   "\"refs_at_delay\":%d,\"refs\":%d,\"begin_refused\":%u,\"stats_buffered\":%u,\"stats_dropped\":%u,"
                   "\"stats_file_bytes\":%u,\"stats_active\":%d",
#ifdef SDLOG_BASE
                   "base",
#else
                   "head",
#endif
                   (unsigned)host_tick(), g_sd_io_errors, g_sd_io_oks, shim_violations(), g_refs_at_delay_violations,
                   g_sd_refs, g_sd_begin_refused, (unsigned)buffered, (unsigned)dropped, (unsigned)file_bytes,
                   active ? 1 : 0);
#ifndef SDLOG_BASE
            sd_logger_acct_t a;
            sd_logger_acct(&a);
            printf(",\"acct\":{\"dropped_ring_bytes\":%llu,\"dropped_rotate_blocked_bytes\":%llu,"
                   "\"dropped_unavailable_bytes\":%llu,\"rolled_back_bytes\":%llu,\"indeterminate_bytes\":%llu,"
                   "\"lost_unwritten_bytes\":%llu,\"retention_evicted_bytes\":%llu,\"dropped_records\":%u,"
                   "\"truncated_records\":%u,\"torn_tail_files\":%u,\"write_err\":%u,\"flush_err\":%u,\"fsync_err\":%u,"
                   "\"close_err\":%u,\"truncate_err\":%u,\"rotate_err\":%u,\"open_err\":%u,"
                   "\"buffered_bytes\":%u,\"quarantined\":%d,\"rotate_backoff\":%d}",
                   (unsigned long long)a.dropped_ring_bytes, (unsigned long long)a.dropped_rotate_blocked_bytes,
                   (unsigned long long)a.dropped_unavailable_bytes, (unsigned long long)a.rolled_back_bytes,
                   (unsigned long long)a.indeterminate_bytes, (unsigned long long)a.lost_unwritten_bytes,
                   (unsigned long long)a.retention_evicted_bytes, a.dropped_records, a.truncated_records,
                   a.torn_tail_files, a.write_err, a.flush_err, a.fsync_err, a.close_err, a.truncate_err,
                   a.rotate_err, a.open_err, (unsigned)a.buffered_bytes,
                   a.quarantined, a.rotate_backoff);
            sd_diag_block_t db;
            sd_diag_get(&db);
            char dj[2048];
            if (sd_diag_render_json(&db, dj, sizeof dj) > 0) printf(",\"diag\":%s", dj);
#endif
            printf("}\n");
#if defined(CONFIG_AMBYTE_EVQ_HIL) && CONFIG_AMBYTE_EVQ_HIL
        } else if (strcmp(cmd, "trace_on") == 0) {
            int rc = hil_sdl_trace_on(false);
            printf("{\"trace_on\":%d}\n", rc);
        } else if (strcmp(cmd, "free_psram") == 0) {
            unsigned long v = 0;
            sscanf(line, "%*s %lu", &v);
            hil_sdl_host_free_psram = (size_t)v;
            printf("{\"free_psram\":%lu}\n", v);
        } else if (strcmp(cmd, "trace_probe") == 0) {
            unsigned long v = 0;
            sscanf(line, "%*s %lu", &v);
            printf("{\"trace_probe\":%d}\n", hil_sdl_trace_probe((size_t)v));
        } else if (strcmp(cmd, "trace_off") == 0) {
            hil_sdl_trace_off();
            printf("{\"trace_off\":1}\n");
        } else if (strcmp(cmd, "trace_drain") == 0) {
            hil_sdl_trace_drain();
            printf("{\"drained\":1}\n");
        } else if (strcmp(cmd, "autoarm") == 0) {
            hil_sdl_autoarm_set();
            printf("{\"autoarm\":1}\n");
        } else if (strcmp(cmd, "power_on") == 0) {
            int v = 0;
            sscanf(line, "%*s %d", &v);
            hil_sdl_host_power_on = v != 0;
            printf("{\"power_on\":%d}\n", v);
        } else if (strcmp(cmd, "boot_hook") == 0) {
            hil_sdl_boot_hook();
            printf("{\"boot_hook\":1}\n");
        } else if (strcmp(cmd, "paused") == 0) {
            printf("{\"paused\":%d}\n", sd_logger_hil_paused() ? 1 : 0);
#endif
        } else if (strcmp(cmd, "quit") == 0) {
            break;
        } else if (cmd[0] != '\0') {
            printf("{\"error\":\"unknown command %s\"}\n", cmd);
        }
        fflush(stdout);
    }
    fflush(stdout);
    if (g_console) fflush(g_console);
    _exit(0);        /* the writer thread may be parked forever (prepare_shutdown) */
}
