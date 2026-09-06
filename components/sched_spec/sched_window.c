/*
 * sched_window.c — gate evaluation: OPEN | CLOSED | UNRESOLVED.
 *
 * Edges resolve against today's local date; when `to` falls before `from`
 * the window wraps midnight and is evaluated as two half-windows (today's
 * [from, midnight) and the tail [midnight, to) of yesterday's window). A sun
 * edge that does not exist (polar day/night, ESP_ERR_NOT_FOUND) makes the
 * whole window UNRESOLVED — the gate maps that through `unresolved: run|skip`
 * — except windows lowered from `day`/`night`, which keep time_sync_is_daytime's
 * polar fallback required by design §Gates.
 */

#include "sched_spec.h"

/* Resolve an edge for the local date containing date_local. */
static bool edge_resolve(const sched_edge_t *e, int64_t date_local, int64_t *out)
{
    if (e->kind == SCHED_EDGE_CLOCK) {
        int y, mo, d;
        time_sync_localtime(date_local, &y, &mo, &d, NULL, NULL, NULL, NULL);
        *out = time_sync_make(y, mo, d, e->hh, e->mm, 0);
        return true;
    }
    int64_t ev;
    if (time_sync_sun_on_date(date_local, e->event, &ev) != ESP_OK) return false;
    *out = ev + e->offset_s; /* the offset may push the edge across midnight; fine */
    return true;
}

static int64_t local_midnight(int64_t now_local)
{
    return now_local - (now_local % 86400);
}

sched_window_state_t sched_window_state(const sched_window_t *win, int64_t now_local)
{
    int64_t day0 = local_midnight(now_local);
    int64_t ft = 0, tt = 0;
    bool ok_f = edge_resolve(&win->from, day0, &ft);
    bool ok_t = edge_resolve(&win->to, day0, &tt);
    if (ok_f && ok_t) {
        if (tt > ft) {
            return (ft <= now_local && now_local < tt)
                       ? SCHED_WINDOW_OPEN : SCHED_WINDOW_CLOSED;
        }
        /* wrap (from == to is a compile error); the now < tt branch is the
         * tail of yesterday's window, so yesterday's `from` is not needed */
        return (now_local >= ft || now_local < tt)
                   ? SCHED_WINDOW_OPEN : SCHED_WINDOW_CLOSED;
    }
    /* Polar/unresolvable. day/night keep is_daytime's fallback-of-true;
     * explicit windows report UNRESOLVED and let the gate policy decide. */
    if (win->hint == SCHED_WIN_DAY) {
        return time_sync_is_daytime(now_local) ? SCHED_WINDOW_OPEN : SCHED_WINDOW_CLOSED;
    }
    if (win->hint == SCHED_WIN_NIGHT) {
        return time_sync_is_daytime(now_local) ? SCHED_WINDOW_CLOSED : SCHED_WINDOW_OPEN;
    }
    return SCHED_WINDOW_UNRESOLVED;
}

bool sched_window_next_open(const sched_window_t *win, int64_t now_local, int64_t *out_local)
{
    if (win == NULL || out_local == NULL) return false;
    int64_t day0 = local_midnight(now_local);
    /* A window opens at its `from` edge by construction; scan today + 2 days
     * (time_sync_until_sun's own horizon) for the first resolvable future one. */
    for (int ahead = 0; ahead <= 2; ahead++) {
        int64_t f = 0;
        if (!edge_resolve(&win->from, day0 + (int64_t)ahead * 86400, &f)) continue;
        if (f > now_local) {
            *out_local = f;
            return true;
        }
    }
    return false;
}
