#include "svc_session.h"

#include <stdio.h>
#include <string.h>

static bool emit(const svc_session_ports_t *p, uint32_t id, const char *json)
{
    char frame[SVC_FRAME_MAX + 1];
    size_t n = svc_frame(frame, sizeof frame, id, json);
    bool ok = n && p->write(p->context, frame, n);
    svc_wipe(frame, sizeof frame);
    return ok;
}
static bool error(svc_engine_t *e, const svc_session_ports_t *p, const char *reason, char *json)
{
    svc_protocol_error(&e->protocol, reason, json);
    return emit(p, 0, json);
}
bool svc_session_run(svc_engine_t *e, const svc_session_ports_t *p)
{
    svc_rx_t rx = {0};
    unsigned char raw[64] = {0};
    char json[SVC_JSON_MAX + 1] = {0};
    bool ok = p->enter(p->context);
    svc_engine_unbind(e);
    if (!ok) goto done;
    /* The store field follows the universal response envelope. A damaged or
     * unavailable store still permits discovery/exit and legacy recovery. */
    if (e->protocol.store->instance)
        snprintf(json, sizeof json, "{\"ok\":true,\"store\":\"%016llx\",\"service\":1,\"state\":\"ready\"}",
                 (unsigned long long)e->protocol.store->instance);
    else strcpy(json, "{\"ok\":true,\"store\":null,\"service\":1,\"state\":\"ready\"}");
    if (!(ok = emit(p, 0, json))) goto done;
    int64_t last_input = p->now_ms(p->context), next_sample = last_input;
    bool draining = false;
    for (;;) {
        int64_t now = p->now_ms(p->context);
        if (now >= next_sample) {
            if (p->sample) p->sample(p->context);
            (void)svc_engine_tick(e);
            next_sample = now + 250;
        }
        if (p->overrun(p->context)) {
            svc_engine_unbind(e); svc_rx_clear(&rx);
            if (!draining && !(ok = error(e, p, "rx_overrun", json))) break;
            draining = true; last_input = now;
        }
        if (now - last_input >= 60000) {
            ok = error(e, p, "session_timeout", json); break;
        }
        const char *reason = svc_rx_timeout(&rx, now);
        if (reason && !(ok = error(e, p, reason, json))) break;
        int received = p->read(p->context, raw, sizeof raw, 50);
        now = p->now_ms(p->context);
        if (received < 0 || received > (int)sizeof raw) { ok = false; break; }
        if (p->overrun(p->context)) {
            svc_engine_unbind(e); svc_rx_clear(&rx);
            if (!draining && !(ok = error(e, p, "rx_overrun", json))) break;
            draining = true; last_input = now;
        }
        if (received) last_input = now;
        if (draining) {
            svc_wipe(raw, sizeof raw);
            /* A quiet boundary, rather than a guessed LF from a lost burst,
             * prevents a suffix from being spliced into a subsequent probe. */
            if (!received && now - last_input >= 100) draining = false;
            continue;
        }
        bool exit = false;
        for (int i = 0; i < received; ++i) {
            char *body; uint32_t id;
            reason = svc_rx_byte(&rx, raw[i], now, &body, &id);
            raw[i] = 0;
            if (reason) {
                if (!(ok = error(e, p, reason, json))) break;
                /* Keep rx.draining after invalid/oversized input. Clearing it
                 * here would admit the tail of the very line just rejected. */
            } else if (body) {
                if (p->overrun(p->context)) {
                    svc_engine_unbind(e); svc_rx_clear(&rx); draining = true;
                    ok = error(e, p, "rx_overrun", json); break;
                }
                exit = svc_engine_dispatch(e, body, json);
                ok = emit(p, id, json);
                svc_rx_clear(&rx);
                if (!ok || exit) break;
            }
        }
        svc_wipe(raw, sizeof raw);
        if (!ok || exit) break;
    }
done:
    svc_engine_unbind(e);
    svc_rx_clear(&rx); svc_wipe(raw, sizeof raw); svc_wipe(json, sizeof json);
    if (p->sample) p->sample(p->context);
    p->leave(p->context);
    return ok;
}
