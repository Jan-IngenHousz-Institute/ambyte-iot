/* Attach-time AMBIT firmware gate (see ambit_attach_gate.h). */
#include "ambit_attach_gate.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)
#else
#define LOCK()   ((void)0)
#define UNLOCK() ((void)0)
#endif

typedef struct {
    char    id[18], fw[16];          /* pair last cleared by the checker */
    bool    cleared;
    int64_t recheck_at;              /* 0 = cleared for good; else re-check after */
    char    retry_id[18], retry_fw[16];
    int64_t retry_at;                /* RETRY: don't re-check this pair before */
    bool    held;
} gate_ch_t;

static gate_ch_t            s_ch[AMBIT_ATTACH_CHANNELS];
static uint8_t              s_pending;   /* channels waiting for the checker */
static bool                 s_queued;    /* a checker pass is queued or running */
static ambit_attach_kick_fn s_kick;

static bool same(const char *a, const char *b) { return strcmp(a, b) == 0; }

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void ambit_attach_gate_set_kick(ambit_attach_kick_fn kick)
{
    LOCK();
    s_kick = kick;
    UNLOCK();
}

bool ambit_attach_gate_allow(uint8_t ch, const char *device_id, const char *fw, int64_t now_ms)
{
    if (ch >= AMBIT_ATTACH_CHANNELS || device_id == NULL || fw == NULL) {
        return true;   /* identity unreadable: cannot be auto-updated, never held */
    }
    bool allow, kick = false;
    ambit_attach_kick_fn fn;
    LOCK();
    gate_ch_t *g = &s_ch[ch];
    fn = s_kick;
    if (g->cleared && same(g->id, device_id) && same(g->fw, fw)) {
        allow = true;
        g->held = false;
        if (g->recheck_at != 0 && now_ms >= g->recheck_at) {
            s_pending |= (uint8_t)(1u << ch);   /* measure meanwhile, re-check in the background */
            g->recheck_at = now_ms + AMBIT_ATTACH_RETRY_MS;
        }
    } else {
        allow = false;
        g->held = true;
        bool waiting = g->retry_at != 0 && now_ms < g->retry_at &&
                       same(g->retry_id, device_id) && same(g->retry_fw, fw);
        if (!waiting) s_pending |= (uint8_t)(1u << ch);
    }
    if (s_pending != 0 && !s_queued && fn != NULL) {
        s_queued = true;   /* claimed before kicking: one pass at a time */
        kick = true;
    }
    UNLOCK();
    if (kick && !fn()) {
        LOCK();
        s_queued = false;  /* worker queue full: the next gate call tries again */
        UNLOCK();
    }
    return allow;
}

uint8_t ambit_attach_gate_pending(void)
{
    LOCK();
    uint8_t p = s_pending;
    UNLOCK();
    return p;
}

void ambit_attach_gate_resolve(uint8_t ch, const char *device_id, const char *fw,
                               ambit_attach_verdict_t verdict, int64_t now_ms)
{
    if (ch >= AMBIT_ATTACH_CHANNELS) return;
    LOCK();
    gate_ch_t *g = &s_ch[ch];
    s_pending &= (uint8_t)~(1u << ch);
    switch (verdict) {
    case AMBIT_ATTACH_CLEAR:
    case AMBIT_ATTACH_CLEAR_RECHECK:
        copy(g->id, sizeof g->id, device_id);
        copy(g->fw, sizeof g->fw, fw);
        g->cleared = device_id != NULL && fw != NULL;
        g->recheck_at = verdict == AMBIT_ATTACH_CLEAR ? 0 : now_ms + AMBIT_ATTACH_RETRY_MS;
        g->retry_at = 0;
        g->held = false;
        break;
    case AMBIT_ATTACH_RETRY:
        copy(g->retry_id, sizeof g->retry_id, device_id);
        copy(g->retry_fw, sizeof g->retry_fw, fw);
        g->retry_at = now_ms + AMBIT_ATTACH_RETRY_MS;
        break;
    case AMBIT_ATTACH_FLASHED:
        /* New firmware means a new identity: the next gate call checks it. */
        g->retry_at = 0;
        break;
    }
    UNLOCK();
}

void ambit_attach_gate_checker_done(void)
{
    LOCK();
    s_queued = false;
    UNLOCK();
}

bool ambit_attach_gate_held(uint8_t ch)
{
    if (ch >= AMBIT_ATTACH_CHANNELS) return false;
    LOCK();
    bool h = s_ch[ch].held;
    UNLOCK();
    return h;
}

void ambit_attach_gate_forget(uint8_t ch)
{
    if (ch >= AMBIT_ATTACH_CHANNELS) return;
    LOCK();
    memset(&s_ch[ch], 0, sizeof s_ch[ch]);
    s_pending &= (uint8_t)~(1u << ch);
    UNLOCK();
}

bool ambit_attach_power_ok(const power_reading_t *p)
{
    if (p == NULL) return false;
    if (p->input_present) return true;
    const unsigned mv = p->battery_mv != 0 ? p->battery_mv : p->system_mv;
    return mv >= AMBIT_ATTACH_BATTERY_FLOOR_MV;
}
