/*
 * sched_due.c — pure due-time model with an injected clock.
 *
 * No FreeRTOS, no wall clock: the runner hands in now_utc and a localize
 * function (the timezone component's offset on device, a fixed-offset stub on
 * the host). All math runs on local Unix seconds via time_sync.
 *
 * Semantics:
 * - Late grace: a slot at D is fireable while L - D ≤ grace, where grace is
 *   the period for `every` (a 1 Hz grid tolerates a 1 s overrun) and 600 s
 *   otherwise (plan: "600 s default"). Older slots are missed.
 * - missed: skip counts the slot(s) in job->skipped and drops them, never a
 *   replay burst; missed: run-once fires a single make-up run (the old
 *   catch_up_once). Only slots whose gate was OPEN at their due time count
 *   or trigger the make-up — the gate is evaluated per occurrence, since a
 *   clock or sun window can open and close many times inside one stale
 *   interval. Slots whose due time fell while the gate was closed advance
 *   silently — they were never runnable, so they are not "missed".
 * - overlap (skip / queue-one / reject) governs execution while a job is
 *   still running; execution belongs to the runner (T3). The model hands out
 *   at most one firing per job per poll.
 * - on_enter: a gated job fires once when its window transitions closed →
 *   open (the legacy script's "immediate sample on entering a phase").
 * - re-anchor: after a wall-clock step the runner calls sched_due_reanchor()
 *   before polling; dues stranded in the future by a backward correction are
 *   recomputed from the new now, past dues keep the missed-slot path, and no
 *   trigger/counter/gate state is re-armed or erased.
 * - DST: dues are absolute local times and only move forward, so the
 *   fall-back double wall minute cannot refire; fired_minute latches the
 *   cron minute as belt-and-braces for the minute-tick path. Spring-forward
 *   slots never materialise on the wall, so a poll always finds them past
 *   grace → counted skipped, not fired.
 */

#include "sched_spec.h"

#include <string.h>

/* plan: late grace is the period for `every`, 600 s default otherwise */
#define LATE_GRACE_DEFAULT_S 600
/* cap on occurrence-counting loops after a clock step; beyond this the exact
 * number stops mattering — the counter exists to surface misconfiguration */
#define SKIP_COUNT_CAP 4096

static int64_t next_trigger_due(const sched_trigger_t *t, int64_t from_local)
{
    switch (t->kind) {
    case SCHED_TRIG_EVERY: {
        int64_t period = t->u.every.period_ms;
        int64_t phase  = t->u.every.phase_ms;
        /* grid in ms, dues quantised up to whole local seconds (the time
         * base is seconds); strictly after from_local */
        int64_t n = (from_local * 1000 - phase) / period + 1;
        int64_t g = n * period + phase;
        return (g + 999) / 1000;
    }
    case SCHED_TRIG_CRON: {
        int64_t out;
        if (sched_cron_next(&t->u.cron, from_local, &out) != ESP_OK) return -1;
        return out;
    }
    case SCHED_TRIG_AT: {
        int64_t w = time_sync_until_clock(from_local, t->u.at.hh, t->u.at.mm, 0);
        return w < 0 ? -1 : from_local + w;
    }
    case SCHED_TRIG_WEEKLY: {
        int64_t w = time_sync_until_weekly(from_local, t->u.weekly.days_mask,
                                           t->u.weekly.hh, t->u.weekly.mm);
        return w < 0 ? -1 : from_local + w;
    }
    case SCHED_TRIG_SUN: {
        int64_t w = time_sync_until_sun(from_local, t->u.sun.event, t->u.sun.offset_s);
        return w < 0 ? -1 : from_local + w; /* -1: polar, no event in 2 days */
    }
    case SCHED_TRIG_BOOT:
    case SCHED_TRIG_DISPATCH:
    default:
        return -1; /* boot is a flag, dispatch is runner-invoked */
    }
}

/* Gate evaluation: UNRESOLVED maps through the window's unresolved: policy
 * (day/night hints already resolved to OPEN/CLOSED inside window_state). */
static bool gate_open_at(const sched_job_t *job, int64_t local)
{
    if (!job->has_window) return true;
    switch (sched_window_state(&job->window, local)) {
    case SCHED_WINDOW_OPEN:   return true;
    case SCHED_WINDOW_CLOSED: return false;
    default: return job->window.unresolved == SCHED_UNRESOLVED_RUN;
    }
}

/* Walk occurrences of t from D up to limit (inclusive), gate-checking each
 * one: only slots whose gate was open at their due time are runnable — they
 * count into *count and set *any_open. A gate can flip many times inside
 * one stale interval, so coalescing arithmetically from the first slot's
 * gate state would miscount (review T2-2: a 10 m grid behind a daily
 * 10:00–11:00 window reported 155 skipped where 11 were runnable). Returns
 * the first due > limit. Bounded by SKIP_COUNT_CAP per call; the caller
 * re-loops, so total work stays linear in stale slots, each iteration a
 * cheap window check. */
static int64_t walk_missed(const sched_trigger_t *t, const sched_job_t *job,
                           int64_t D, int64_t limit,
                           uint32_t *count, bool *any_open)
{
    uint32_t iter = 0;
    int64_t cur = D;
    while (cur >= 0 && cur <= limit && iter < SKIP_COUNT_CAP) {
        iter++;
        if (gate_open_at(job, cur)) {
            (*count)++;
            *any_open = true;
        }
        int64_t next = next_trigger_due(t, cur);
        if (next <= cur) break; /* defensive: dues must advance */
        cur = next;
    }
    return cur;
}

void sched_due_init(sched_due_t *d, const sched_program_t *prog,
                    sched_localize_fn localize, void *ctx, int64_t now_utc)
{
    memset(d->jobs, 0, sizeof(d->jobs));
    d->prog = prog;
    d->localize = localize;
    d->localize_ctx = ctx;
    int64_t L = localize(ctx, now_utc);
    for (int j = 0; j < prog->job_count; j++) {
        const sched_job_t *job = &prog->jobs[j];
        sched_due_job_t *dj = &d->jobs[j];
        for (int t = 0; t < SCHED_SPEC_MAX_TRIGGERS; t++) dj->due[t] = -1;
        for (int t = 0; t < job->trigger_count; t++) {
            const sched_trigger_t *tr = &job->triggers[t];
            if (tr->kind == SCHED_TRIG_BOOT) {
                dj->boot_pending = 1; /* runner inits after clock trust */
                continue;
            }
            dj->due[t] = next_trigger_due(tr, L);
        }
        dj->fired_minute = -1;
        /* seed the gate state: init is not an "entry", nothing fires yet */
        dj->gate_open = gate_open_at(job, L) ? 1 : 0;
    }
}

uint32_t sched_due_poll(sched_due_t *d, int64_t now_utc)
{
    const sched_program_t *prog = d->prog;
    int64_t L = d->localize(d->localize_ctx, now_utc);
    uint32_t mask = 0;
    for (int j = 0; j < prog->job_count; j++) {
        const sched_job_t *job = &prog->jobs[j];
        sched_due_job_t *dj = &d->jobs[j];
        bool gate = gate_open_at(job, L);
        bool fired = false;

        /* gate entry: fire once on closed → open */
        if (job->has_window && job->on_enter && gate && !dj->gate_open) fired = true;
        dj->gate_open = gate ? 1 : 0;

        if (dj->boot_pending && gate) {
            fired = true;
            dj->boot_pending = 0;
        }

        for (int i = 0; i < job->trigger_count; i++) {
            const sched_trigger_t *tr = &job->triggers[i];
            int64_t D = dj->due[i];
            int64_t grace = (tr->kind == SCHED_TRIG_EVERY)
                                ? tr->u.every.period_ms / 1000
                                : LATE_GRACE_DEFAULT_S;
            while (D >= 0 && D <= L) {
                if (L - D > grace) {
                    /* stale slot(s): walk each occurrence up to the grace
                     * horizon and gate-check it. skip: runnable slots count
                     * into job->skipped. run-once: a single make-up fire iff
                     * at least one missed slot was runnable. The walk lands
                     * on the first due past the horizon, which may still be
                     * ≤ L and fireable — re-loop instead of dropping it. */
                    uint32_t n = 0;
                    bool any_open = false;
                    D = walk_missed(tr, job, D, L - grace, &n, &any_open);
                    if (job->missed == SCHED_MISSED_RUN_ONCE) {
                        if (any_open) fired = true;
                    } else {
                        dj->skipped += n;
                    }
                    continue;
                }
                if (!gate) {
                    /* gate closed: the slot passes silently (never "skipped");
                     * advance grid dues so they cannot pile up behind it */
                    D = next_trigger_due(tr, L);
                    break;
                }
                if (tr->kind == SCHED_TRIG_CRON) {
                    if (dj->fired_minute == L / 60) { /* fall-back double minute */
                        D = next_trigger_due(tr, L);
                        break;
                    }
                    dj->fired_minute = L / 60;
                }
                fired = true;
                D = next_trigger_due(tr, L > D ? L : D);
                break;
            }
            dj->due[i] = D;
        }
        if (fired) {
            mask |= 1u << j;
            dj->runs++;
        }
    }
    return mask;
}

int64_t sched_due_next(const sched_due_t *d, int64_t now_utc)
{
    const sched_program_t *prog = d->prog;
    int64_t L = d->localize(d->localize_ctx, now_utc);
    int64_t best = -1;
    for (int j = 0; j < prog->job_count; j++) {
        const sched_job_t *job = &prog->jobs[j];
        const sched_due_job_t *dj = &d->jobs[j];
        bool gate = gate_open_at(job, L);
        int64_t cand = -1;
        if (gate) {
            if (dj->boot_pending) cand = L;
            for (int i = 0; i < job->trigger_count; i++) {
                int64_t D = dj->due[i];
                if (D < 0) continue;
                int64_t c = D <= L ? L : D; /* overdue → act now */
                if (cand < 0 || c < cand) cand = c;
            }
        } else if (job->has_window) {
            /* closed gate: the next thing that can happen is the window
             * opening (on_enter fire, then grid resumption at poll) */
            int64_t open_t;
            if (sched_window_next_open(&job->window, L, &open_t)) cand = open_t;
        }
        if (cand >= 0 && (best < 0 || cand < best)) best = cand;
    }
    return best;
}

void sched_due_reanchor(sched_due_t *d, int64_t now_utc)
{
    const sched_program_t *prog = d->prog;
    int64_t L = d->localize(d->localize_ctx, now_utc);
    for (int j = 0; j < prog->job_count; j++) {
        const sched_job_t *job = &prog->jobs[j];
        sched_due_job_t *dj = &d->jobs[j];
        for (int i = 0; i < job->trigger_count; i++) {
            /* Only dues anchored AHEAD of the new now are stale in the way a
             * backward correction produces; recompute them from the new local
             * now. Dues at/past now stay for poll()'s missed-slot accounting
             * (forward step → counted skips or one make-up run). */
            if (dj->due[i] > L) {
                dj->due[i] = next_trigger_due(&job->triggers[i], L);
            }
        }
        /* boot_pending, skipped, runs, fired_minute and gate_open are
         * deliberately NOT touched: a consumed boot trigger stays consumed,
         * counters survive, and no spurious on_enter edge is manufactured. */
    }
}
