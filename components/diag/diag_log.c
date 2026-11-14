/*
 * diag_log.c — the DRAIN half of ambyte.log/1 capture (task context only:
 * the sync_runner watchdog task) plus the shared state. The hook half that
 * runs on every logging task is diag_log_hook.c; the contract and the
 * hook-chain order are documented in include/diag_log.h.
 *
 * Everything the hook touches is static and fixed-size: ~4.8 KiB of .bss
 * (24 × ~200 B lines), no heap, no locks other than g_diag_log_mux.
 */
#include "diag_log.h"

#include <string.h>

#include "diag_json.h"
#include "diag_log_port.h"
#include "diag_log_priv.h"

#ifndef DIAG_LOG_HOST
portMUX_TYPE g_diag_log_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

diag_log_state_t g_dl = {.clock_valid_since_us = -1};

/* ── drain (task context) ──────────────────────────────────────────────── */

void diag_log_note_clock_valid(int64_t now_uptime_us)
{
    diag_port_lock();
    if (g_dl.clock_valid_since_us < 0) g_dl.clock_valid_since_us = now_uptime_us;
    diag_port_unlock();
}

bool diag_log_seal(diag_log_batch_t *b, int64_t now_uptime_us)
{
    if (b == NULL) return false;
    memset(b, 0, sizeof *b);
    bool sealed = false;
    diag_port_lock();
    const bool too_soon = g_dl.have_record &&
                          now_uptime_us - g_dl.last_record_us < DIAG_LOG_MIN_INTERVAL_US;
    const bool empty = g_dl.count == 0 && g_dl.dropped == 0 && g_dl.rate_limited == 0 &&
                       g_dl.unsupported_fmt == 0;
    if (!too_soon && !empty && g_dl.sealed == 0) {
        g_dl.sealed = g_dl.count;
        b->n = g_dl.count;
        b->dropped = g_dl.dropped;
        b->rate_limited = g_dl.rate_limited;
        b->unsupported_fmt = g_dl.unsupported_fmt;
        sealed = true;
    }
    diag_port_unlock();
    return sealed;
}

/* Room kept for the closing `],"dropped":N,"rate_limited":N,"unsupported_fmt":N}`
 * at u32 max (79 B). */
#define DIAG_LOG_TAIL_RESERVE 96U

int diag_log_render(diag_log_batch_t *b, const diag_log_render_ctx_t *ctx, char *out, size_t cap)
{
    if (b == NULL || ctx == NULL || out == NULL || cap == 0) return -1;
    const size_t limit = cap < DIAG_LOG_RECORD_CAP + 1U ? cap : DIAG_LOG_RECORD_CAP + 1U;
    diag_port_lock();
    const int64_t valid_since = g_dl.clock_valid_since_us;
    diag_port_unlock();
    diag_jw_t w;
    diag_jw_init(&w, out, limit);
    /* schema FIRST: the publisher routes canonical v3 families by this exact
     * prefix (payload_v3_is_canonical_object) without reparsing. */
    diag_jw_raw(&w, "{\"schema\":\"ambyte.log/1\",\"measure_id\":%lld,\"device\":",
                (long long)ctx->measure_id);
    diag_jw_str(&w, ctx->device != NULL ? ctx->device : "");
    diag_jw_raw(&w, ",\"tag\":\"LOG\",\"time\":{\"observed_utc\":%lld},\"boot_count\":%lu,\"lines\":[",
                (long long)ctx->observed_utc_ms, (unsigned long)ctx->boot_count);
    if (!w.ok) return -1;
    b->used = 0;
    for (uint32_t i = 0; i < b->n; i++) {
        /* Sealed entries are immutable and g_dl.tail moves only in commit (this
         * task), so they are read without the lock. */
        const diag_line_t *e = &g_dl.ring[(g_dl.tail + i) % DIAG_LOG_RING_LINES];
        const size_t mark = w.len;
        diag_jw_raw(&w, "%s{\"uptime_ms\":%lld,\"utc_ms\":", i ? "," : "",
                    (long long)(e->uptime_us / 1000));
        if (ctx->clock_valid && valid_since >= 0 && e->uptime_us >= valid_since) {
            /* Back-computed from the drain-time clock along the monotonic
             * uptime axis: exact unless the wall clock was stepped in between,
             * in which case it reflects the corrected clock. */
            diag_jw_raw(&w, "%lld", (long long)(ctx->now_utc_ms - (ctx->now_uptime_us - e->uptime_us) / 1000));
        } else {
            diag_jw_raw(&w, "null");
        }
        diag_jw_raw(&w, ",\"level\":\"%c\",\"tag\":", e->level);
        diag_jw_str(&w, e->tag);
        diag_jw_raw(&w, ",\"msg\":");
        diag_jw_str(&w, e->msg);
        diag_jw_raw(&w, ",\"repeat\":%lu}", (unsigned long)e->repeat);
        if (!w.ok || w.len + DIAG_LOG_TAIL_RESERVE >= limit) {
            diag_jw_rewind(&w, mark);     /* this line waits for the next record */
            break;
        }
        b->used++;
    }
    diag_jw_raw(&w, "],\"dropped\":%lu,\"rate_limited\":%lu,\"unsupported_fmt\":%lu}",
                (unsigned long)b->dropped, (unsigned long)b->rate_limited,
                (unsigned long)b->unsupported_fmt);
    return w.ok ? (int)w.len : -1;
}

void diag_log_commit(const diag_log_batch_t *b, int64_t now_uptime_us)
{
    if (b == NULL) return;
    diag_port_lock();
    const uint32_t used = b->used <= g_dl.count ? b->used : g_dl.count;
    g_dl.tail = (g_dl.tail + used) % DIAG_LOG_RING_LINES;
    g_dl.count -= used;
    g_dl.sealed = 0;
    /* Counters may have grown since the seal: subtract only what went out. */
    g_dl.dropped = g_dl.dropped >= b->dropped ? g_dl.dropped - b->dropped : 0;
    g_dl.rate_limited = g_dl.rate_limited >= b->rate_limited ? g_dl.rate_limited - b->rate_limited : 0;
    g_dl.unsupported_fmt = g_dl.unsupported_fmt >= b->unsupported_fmt ? g_dl.unsupported_fmt - b->unsupported_fmt : 0;
    g_dl.have_record = true;
    g_dl.last_record_us = now_uptime_us;
    sat_inc(&g_dl.total_records);
    diag_port_unlock();
}

void diag_log_unseal(void)
{
    diag_port_lock();
    g_dl.sealed = 0;
    diag_port_unlock();
}

void diag_log_stats(diag_log_stats_t *out)
{
    if (out == NULL) return;
    diag_port_lock();
    out->buffered = g_dl.count;
    out->dropped = g_dl.dropped;
    out->rate_limited = g_dl.rate_limited;
    out->unsupported_fmt = g_dl.unsupported_fmt;
    out->total_captured = g_dl.total_captured;
    out->total_repeats = g_dl.total_repeats;
    out->total_records = g_dl.total_records;
    diag_port_unlock();
}

#ifdef DIAG_LOG_HOST
void diag_log_reset_for_test(void)
{
    diag_port_lock();
    memset(g_dl.ring, 0, sizeof g_dl.ring);
    memset(g_dl.active, 0, sizeof g_dl.active);
    g_dl.tail = g_dl.count = g_dl.sealed = 0;
    g_dl.dropped = g_dl.rate_limited = g_dl.unsupported_fmt = 0;
    g_dl.total_captured = g_dl.total_repeats = g_dl.total_records = 0;
    memset(&g_dl.min_bucket, 0, sizeof g_dl.min_bucket);
    memset(&g_dl.hour_bucket, 0, sizeof g_dl.hour_bucket);
    g_dl.have_record = false;
    g_dl.last_record_us = 0;
    g_dl.clock_valid_since_us = -1;
    diag_port_unlock();
    g_dl.next = NULL;
}

void diag_log_flush_for_test(void)
{
    diag_port_lock();
    g_dl.tail = g_dl.count = g_dl.sealed = 0;
    g_dl.dropped = g_dl.rate_limited = 0;
    diag_port_unlock();
}
#endif
