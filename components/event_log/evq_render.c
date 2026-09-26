/*
 * evq_render.c — stable names and the `evlog` CLI rendering of evlog_health_t.
 *
 * Pure C (no FreeRTOS, no I/O) so the host harness renders exactly what the
 * device prints. The renderer refuses (-1) rather than truncating: a cut-off
 * status line that silently drops "storage_blocked=1" is worse than an error.
 */
#include "event_log.h"

#include <stdarg.h>
#include <stdio.h>

const char *event_log_sd_state_name(uint8_t state)
{
    switch (state) {
    case EVQ_SD_ABSENT:          return "absent";
    case EVQ_SD_OK:              return "ok";
    case EVQ_SD_LOST:            return "lost";
    case EVQ_SD_PARKED:          return "parked";
    case EVQ_SD_FULL:            return "full";
    case EVQ_SD_MISMATCH:        return "mismatch";
    case EVQ_SD_BACKLOG_MISSING: return "backlog_missing";
    case EVQ_SD_BACKLOG_CORRUPT: return "backlog_corrupt";
    default:                     return "unknown";
    }
}

const char *event_log_block_name(uint8_t block)
{
    switch (block) {
    case EVQ_BLOCK_NONE:            return "none";
    case EVQ_BLOCK_SD_ABSENT:       return "sd_absent";
    case EVQ_BLOCK_SD_LOST:         return "sd_lost";
    case EVQ_BLOCK_SD_PARKED:       return "sd_parked";
    case EVQ_BLOCK_SD_MISMATCH:     return "sd_mismatch";
    case EVQ_BLOCK_BACKLOG_MISSING: return "backlog_missing";
    case EVQ_BLOCK_BACKLOG_CORRUPT: return "backlog_corrupt";
    default:                        return "unknown";
    }
}

const char *event_log_blocked_reason_name(uint8_t reason)
{
    switch (reason) {
    case EVQ_BLOCKED_NONE:             return "none";
    case EVQ_BLOCKED_SD_UNAVAILABLE:   return "flash_full_sd_unavailable";
    case EVQ_BLOCKED_SD_FULL:          return "flash_full_sd_full";
    case EVQ_BLOCKED_SD_ERROR:         return "flash_full_sd_error";
    case EVQ_BLOCKED_SD_MISMATCH:      return "flash_full_sd_mismatch";
    case EVQ_BLOCKED_BACKLOG_WAITING:  return "flash_full_backlog_waiting";
    case EVQ_BLOCKED_INDEX_CAP:        return "flash_full_index_cap";
    case EVQ_BLOCKED_TRANSFER_PENDING: return "flash_full_transfer_pending";
    default:                           return "unknown";
    }
}

const char *event_log_medium_name(uint8_t medium)
{
    switch (medium) {
    case EVQ_MEDIUM_FLASH: return "flash";
    case EVQ_MEDIUM_SD:    return "sd";
    default:               return "none";
    }
}

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ok;
} evq_w_t;

static void w_add(evq_w_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void w_add(evq_w_t *w, const char *fmt, ...)
{
    if (!w->ok) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) { w->ok = false; return; }
    w->len += (size_t)n;
}

int evq_render_health_text(const evlog_health_t *h, char *buf, size_t cap)
{
    if (h == NULL || buf == NULL || cap == 0) return -1;
    evq_w_t w = { .buf = buf, .cap = cap, .len = 0, .ok = true };
    buf[0] = '\0';
    /* pending_exact=0 renders the count as an explicit floor ("pending>=N"). */
    w_add(&w, "evlog: available=%d pending%s%lld pending_exact=%d deliverable_pending=%lld\r\n",
          h->available ? 1 : 0, h->pending_exact ? "=" : ">=", (long long)h->pending,
          h->pending_exact ? 1 : 0, (long long)h->deliverable_pending);
    w_add(&w, "  flash_pending=%lld sd_pending=%lld reimport_pending=%lld next_id=%lld last_acked_id=%lld\r\n",
          (long long)h->flash_pending, (long long)h->sd_pending, (long long)h->reimport_pending,
          (long long)h->next_id, (long long)h->last_acked_id);
    w_add(&w, "  cursor=ev-%06u tail=ev-%06u sd_state=%s head_block=%s\r\n",
          (unsigned)h->rd_seq, (unsigned)h->tail_seq, event_log_sd_state_name(h->sd_state),
          event_log_block_name(h->head_block));
    w_add(&w, "  storage_blocked=%d blocked_reason=%s write_full=%d\r\n",
          h->storage_blocked ? 1 : 0, event_log_blocked_reason_name(h->blocked_reason),
          h->write_full ? 1 : 0);
    w_add(&w, "  refused_full=%lld refused_media=%lld refused_too_large=%lld refused_unavailable=%lld dropped=%lld\r\n",
          (long long)h->refused_full, (long long)h->refused_media, (long long)h->refused_too_large,
          (long long)h->refused_unavailable, (long long)h->dropped);
    w_add(&w, "  quarantined_poison=%lld quarantined_malformed=%lld skipped_unindexed_gap=%lld corrupt_detected=%lld corrupt_medium=%s skipped=%lld\r\n",
          (long long)h->quarantined_poison, (long long)h->quarantined_malformed,
          (long long)h->skipped_unindexed_gap, (long long)h->corrupt_detected,
          event_log_medium_name(h->corrupt_medium), (long long)h->skipped);
    w_add(&w, "  spool_files=%u spool_errors=%u mirror_used=%u reclaimed=%u archived=%u reimported=%u pressure_notifies=%u sd_bursts=%u index=%u/%u\r\n",
          (unsigned)h->spool_files, (unsigned)h->spool_errors, (unsigned)h->mirror_used,
          (unsigned)h->reclaimed_files, (unsigned)h->archived_files, (unsigned)h->reimported_files,
          (unsigned)h->pressure_notifies, (unsigned)h->sd_bursts,
          (unsigned)h->index_segments, (unsigned)h->index_cap);
    w_add(&w, "  sd_retired_names=%u sd_bad_copies=%u\r\n", (unsigned)h->sd_retired_names, (unsigned)h->sd_bad_copies);
    return w.ok ? (int)w.len : -1;
}
