#include "mp2731_aico.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void aico_init(aico_t *s)
{
    memset(s, 0, sizeof *s);
    s->enabled_seen = -1;
}

aico_action_t aico_step(aico_t *s, int64_t now_ms, bool veto)
{
    if (!s->started) {
        /* The boot's first read starts the clock: AICO_EN survives an ESP
         * reset (it is a charger register), and a limit learned before the
         * reboot gets a full interval like any other. */
        s->started = true;
        s->next_ms = now_ms + AICO_RESET_INTERVAL_MS;
        return AICO_NONE;
    }
    if (now_ms < s->next_ms) return AICO_NONE;
    if (veto) {
        if (!s->waiting_veto) {
            s->waiting_veto = true;
            s->vetoed++;
        }
        return AICO_NONE;
    }
    s->waiting_veto = false;
    return AICO_CHECK;
}

void aico_note_result(aico_t *s, int64_t now_ms, aico_result_t r)
{
    s->started = true;
    s->waiting_veto = false;
    s->checks++;
    switch (r) {
    case AICO_RESULT_RESET:
        s->resets++;
        s->has_reset = true;
        s->last_reset_ms = now_ms;
        s->enabled_seen = 1;
        s->next_ms = now_ms + AICO_RESET_INTERVAL_MS;
        break;
    case AICO_RESULT_OFF:
        s->enabled_seen = 0;
        s->next_ms = now_ms + AICO_RESET_INTERVAL_MS;
        break;
    case AICO_RESULT_FAILED:
        s->failed++;
        s->next_ms = now_ms + AICO_RETRY_MS;
        break;
    }
}

void aico_note_enabled(aico_t *s, int64_t now_ms, bool enabled)
{
    s->enabled_seen = enabled ? 1 : 0;
    if (enabled) {
        s->started = true;
        s->waiting_veto = false;
        s->next_ms = now_ms + AICO_RESET_INTERVAL_MS;
    }
}

uint32_t aico_next_in_s(const aico_t *s, int64_t now_ms)
{
    if (!s->started || s->next_ms <= now_ms) return 0;
    const int64_t ms = s->next_ms - now_ms;
    return (uint32_t)(ms / 1000 + (ms % 1000 != 0));
}

static void append(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    if (*used >= cap) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + *used, cap - *used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap - *used) { *used = cap; return; }
    *used += (size_t)n;
}

int aico_render(const aico_t *s, const uint8_t *regs, uint32_t mask, char *out, size_t cap)
{
    if (s == NULL || out == NULL || cap == 0) return -1;
    size_t n = 0;
    append(out, cap, &n, "{");
    /* Scan-derived fields first, each only when its register was read. */
    const char *sep = "";
    if (regs != NULL && (mask & (1u << 0x02)) != 0u) {
        append(out, cap, &n, "\"enabled\":%s", (regs[0x02] & 0x01u) ? "true" : "false");
        sep = ",";
    }
    if (regs != NULL && (mask & (1u << 0x14)) != 0u) {
        append(out, cap, &n, "%s\"iin_dpm_ma\":%u", sep, 100u + (regs[0x14] & 0x3Fu) * 50u);
        sep = ",";
    }
    if (regs != NULL && (mask & (1u << 0x15)) != 0u) {
        append(out, cap, &n, "%s\"aico_stat\":%s", sep, (regs[0x15] & 0x80u) ? "true" : "false");
        sep = ",";
    }
    append(out, cap, &n, "%s\"resets\":%u,\"failed\":%u,\"last_reset_ms\":", sep,
           (unsigned)s->resets, (unsigned)s->failed);
    if (s->has_reset) append(out, cap, &n, "%lld}", (long long)s->last_reset_ms);
    else append(out, cap, &n, "null}");
    if (n >= cap) { out[0] = '\0'; return -1; }
    return (int)n;
}
