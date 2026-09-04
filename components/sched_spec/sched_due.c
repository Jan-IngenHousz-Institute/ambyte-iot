/*
 * sched_due.c — pure dual-clock due-time model.
 *
 * Armed instants live in monotonic microseconds. Wall UTC is sampled only at
 * init/re-anchor; between anchors it is projected from monotonic elapsed time.
 * This is load-bearing for `every`: a small RTC correction or NTP slew cannot
 * stretch/compress the grid. Cron/at/weekly/sun retain their paired local-wall
 * anchor so their successor can be resolved after a fire and so clock-step,
 * DST, gate and missed-slot behavior stays explicit.
 *
 * The runner compares real wall UTC with sched_due_project_wall_utc() at least
 * every 60 s. A difference over 2 s calls sched_due_reanchor_mono(). The model
 * also notices a timezone-offset transition on its projected UTC timeline and
 * reprojects wall triggers; that preserves spring-gap/fall-repeat semantics
 * without making `every` depend on local wall time.
 */

#include "sched_spec.h"

#include <limits.h>
#include <string.h>

#define US_PER_S 1000000LL
#define LATE_GRACE_DEFAULT_S 600

/* One hour of a gated 1 Hz grid. Ungated every grids use arithmetic and closed
 * window stretches jump to their next opening; once this budget is exhausted,
 * skipped is deliberately a lower bound and skipped_saturated says so. */
#define SCHED_MISSED_WALK_CAP 3600

static int64_t next_trigger_due(const sched_trigger_t *t, int64_t from_local)
{
    switch (t->kind) {
    case SCHED_TRIG_EVERY: {
        int64_t period = t->u.every.period_ms;
        int64_t phase  = t->u.every.phase_ms;
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
        int64_t w = time_sync_until_sun(from_local, t->u.sun.event,
                                        t->u.sun.offset_s);
        return w < 0 ? -1 : from_local + w;
    }
    case SCHED_TRIG_BOOT:
    case SCHED_TRIG_DISPATCH:
    default:
        return -1;
    }
}

static bool gate_open_at(const sched_job_t *job, int64_t local)
{
    if (!job->has_window) return true;
    switch (sched_window_state(&job->window, local)) {
    case SCHED_WINDOW_OPEN:   return true;
    case SCHED_WINDOW_CLOSED: return false;
    default: return job->window.unresolved == SCHED_UNRESOLVED_RUN;
    }
}

static int64_t floor_us_to_s(int64_t us)
{
    int64_t q = us / US_PER_S;
    if (us < 0 && us % US_PER_S != 0) q--;
    return q;
}

int64_t sched_due_project_wall_utc(const sched_due_t *d, int64_t mono_us)
{
    return d->anchor_wall_utc + floor_us_to_s(mono_us - d->anchor_mono_us);
}

static int64_t local_at_mono(const sched_due_t *d, int64_t mono_us)
{
    return d->localize(d->localize_ctx,
                       sched_due_project_wall_utc(d, mono_us));
}

static int32_t offset_at_mono(const sched_due_t *d, int64_t mono_us)
{
    int64_t utc = sched_due_project_wall_utc(d, mono_us);
    int64_t local = d->localize(d->localize_ctx, utc);
    int64_t off = local - utc;
    if (off > INT32_MAX) return INT32_MAX;
    if (off < INT32_MIN) return INT32_MIN;
    return (int32_t)off;
}

static int64_t project_local(const sched_due_t *d, int64_t target_local,
                             int64_t ref_mono_us)
{
    if (target_local < 0) return SCHED_DUE_NONE_US;
    return ref_mono_us + (target_local - local_at_mono(d, ref_mono_us)) * US_PER_S;
}

static void set_wall_due(const sched_due_t *d, sched_due_job_t *job, int idx,
                         int64_t local, int64_t ref_mono_us)
{
    job->wall_due_local[idx] = local;
    job->due_us[idx] = project_local(d, local, ref_mono_us);
}

static void sync_every_wall_due(const sched_due_t *d, sched_due_job_t *job,
                                int idx, int64_t ref_mono_us)
{
    if (job->due_us[idx] == SCHED_DUE_NONE_US) {
        job->wall_due_local[idx] = -1;
        return;
    }
    /* This paired value is not the armed clock. It exists for status and for a
     * later explicit wall re-anchor. Restore the current localization after
     * probing a future instant because the device callback also refreshes the
     * sun calculator's current UTC offset. */
    job->wall_due_local[idx] = local_at_mono(d, job->due_us[idx]);
    (void)local_at_mono(d, ref_mono_us);
}

static bool trigger_can_resolve(const sched_trigger_t *trigger)
{
    return trigger->kind != SCHED_TRIG_BOOT &&
           trigger->kind != SCHED_TRIG_DISPATCH;
}

static void accumulate_skipped(sched_due_job_t *job, uint64_t count)
{
    if (count > UINT32_MAX || UINT32_MAX - job->skipped < count) {
        job->skipped = UINT32_MAX;
        job->skipped_saturated = 1;
    } else {
        job->skipped += (uint32_t)count;
    }
}

/* Wall-trigger stale walk. D and limit are paired local-wall anchors; the
 * monotonic comparison that decided the slot was stale happens in poll(). */
static int64_t walk_wall_missed(const sched_trigger_t *t,
                                const sched_job_t *job,
                                int64_t D, int64_t limit,
                                uint32_t *budget, uint32_t *count,
                                bool *any_open, bool *saturated)
{
    int64_t cur = D;
    while (cur >= 0 && cur <= limit) {
        if (*budget == 0) {
            *saturated = true;
            if (job->missed == SCHED_MISSED_RUN_ONCE && !*any_open) {
                if (!job->has_window || gate_open_at(job, cur)) {
                    *any_open = true;
                } else {
                    int64_t open_t;
                    if (sched_window_next_open(&job->window, cur, &open_t) &&
                        open_t <= limit) *any_open = true;
                }
            }
            return next_trigger_due(t, limit);
        }
        (*budget)--;
        if (gate_open_at(job, cur)) {
            (*count)++;
            *any_open = true;
        } else if (job->has_window) {
            int64_t open_t;
            if (sched_window_next_open(&job->window, cur, &open_t) && open_t > cur) {
                if (open_t > limit) return next_trigger_due(t, limit);
                int64_t nxt = next_trigger_due(t, open_t - 1);
                if (nxt > cur) { cur = nxt; continue; }
            }
        }
        int64_t next = next_trigger_due(t, cur);
        if (next <= cur) break;
        cur = next;
    }
    return cur;
}

static int64_t first_every_after(int64_t due_us, int64_t limit_us,
                                 int64_t period_us)
{
    if (due_us > limit_us) return due_us;
    /* Unsigned subtraction represents the non-negative distance without UB
     * when a large forward wall correction projected due_us below zero. */
    uint64_t distance = (uint64_t)limit_us - (uint64_t)due_us;
    uint64_t n = distance / (uint64_t)period_us + 1u;
    if (n > (uint64_t)INT64_MAX / (uint64_t)period_us) return INT64_MAX;
    int64_t advance = (int64_t)n * period_us;
    if (due_us >= 0 && advance > INT64_MAX - due_us) return INT64_MAX;
    /* Negative armed instants are valid after a forward wall re-anchor. Adding
     * a non-negative value ≤ INT64_MAX to them cannot overflow. */
    return due_us + advance;
}

/* Gated `every` stale walk in the monotonic domain. Gate checks translate each
 * scheduled instant through the anchored wall projection. Closed intervals
 * retain T2's jump-to-next-opening optimization; the per-job budget remains a
 * hard ceiling even when an offset transition makes that projection awkward. */
static int64_t walk_every_missed(const sched_due_t *d,
                                 const sched_trigger_t *trigger,
                                 const sched_job_t *job,
                                 int64_t due_us, int64_t limit_us,
                                 uint32_t *budget, uint32_t *count,
                                 bool *any_open, bool *saturated)
{
    const int64_t period_us = trigger->u.every.period_ms * 1000;
    int64_t cur = due_us;
    while (cur != SCHED_DUE_NONE_US && cur <= limit_us) {
        int64_t local = local_at_mono(d, cur);
        if (*budget == 0) {
            *saturated = true;
            if (job->missed == SCHED_MISSED_RUN_ONCE && !*any_open) {
                if (!job->has_window || gate_open_at(job, local)) {
                    *any_open = true;
                } else {
                    int64_t open_t;
                    if (sched_window_next_open(&job->window, local, &open_t)) {
                        int64_t limit_local = local_at_mono(d, limit_us);
                        if (open_t <= limit_local) *any_open = true;
                    }
                }
            }
            return first_every_after(cur, limit_us, period_us);
        }
        (*budget)--;
        if (gate_open_at(job, local)) {
            (*count)++;
            *any_open = true;
        } else if (job->has_window) {
            int64_t open_t;
            if (sched_window_next_open(&job->window, local, &open_t) &&
                open_t > local) {
                int64_t delta_us = (open_t - local) * US_PER_S;
                int64_t slots = (delta_us + period_us - 1) / period_us;
                int64_t jump = cur + slots * period_us;
                if (jump > cur) { cur = jump; continue; }
            }
        }
        if (cur >= 0 && INT64_MAX - cur < period_us) return INT64_MAX;
        cur += period_us;
    }
    return cur;
}

void sched_due_init_mono(sched_due_t *d, const sched_program_t *prog,
                         sched_localize_fn localize, void *ctx,
                         int64_t now_utc, int64_t now_mono_us)
{
    memset(d, 0, sizeof(*d));
    d->prog = prog;
    d->localize = localize;
    d->localize_ctx = ctx;
    d->anchor_wall_utc = now_utc;
    d->anchor_mono_us = now_mono_us;
    d->anchor_offset_s = (int32_t)(localize(ctx, now_utc) - now_utc);
    int64_t L = localize(ctx, now_utc);

    for (int j = 0; j < prog->job_count; j++) {
        const sched_job_t *job = &prog->jobs[j];
        sched_due_job_t *dj = &d->jobs[j];
        for (int t = 0; t < SCHED_SPEC_MAX_TRIGGERS; t++) {
            dj->wall_due_local[t] = -1;
            dj->due_us[t] = SCHED_DUE_NONE_US;
        }
        for (int t = 0; t < job->trigger_count; t++) {
            const sched_trigger_t *tr = &job->triggers[t];
            if (tr->kind == SCHED_TRIG_BOOT) {
                dj->boot_pending = 1;
            } else {
                set_wall_due(d, dj, t, next_trigger_due(tr, L), now_mono_us);
            }
        }
        dj->fired_minute = -1;
        dj->gate_open = gate_open_at(job, L) ? 1 : 0;
    }
}

void sched_due_init(sched_due_t *d, const sched_program_t *prog,
                    sched_localize_fn localize, void *ctx, int64_t now_utc)
{
    sched_due_init_mono(d, prog, localize, ctx, now_utc, now_utc * US_PER_S);
}

void sched_due_reanchor_mono(sched_due_t *d, int64_t now_utc,
                             int64_t now_mono_us)
{
    const sched_program_t *prog = d->prog;
    int64_t L = d->localize(d->localize_ctx, now_utc);
    d->anchor_wall_utc = now_utc;
    d->anchor_mono_us = now_mono_us;
    d->anchor_offset_s = (int32_t)(L - now_utc);

    for (int j = 0; j < prog->job_count; j++) {
        const sched_job_t *job = &prog->jobs[j];
        sched_due_job_t *dj = &d->jobs[j];
        for (int i = 0; i < job->trigger_count; i++) {
            const sched_trigger_t *tr = &job->triggers[i];
            int64_t wall_due = dj->wall_due_local[i];
            /* Backward correction: a future due belongs to the obsolete larger
             * wall frame, so re-resolve it. Forward correction: keep the past
             * wall anchor and project it behind `now`; poll() then applies the
             * exact T2 missed/run-once accounting. */
            if (wall_due > L) wall_due = next_trigger_due(tr, L);
            set_wall_due(d, dj, i, wall_due, now_mono_us);
        }
    }
}

void sched_due_reanchor(sched_due_t *d, int64_t now_utc)
{
    sched_due_reanchor_mono(d, now_utc, now_utc * US_PER_S);
}

uint32_t sched_due_poll_mono(sched_due_t *d, int64_t now_mono_us)
{
    /* A timezone offset edge is not an RTC step, but wall triggers on the far
     * side need reprojection. Use projected UTC, never a fresh wall sample;
     * `every` remains owned by monotonic time. */
    int32_t off = offset_at_mono(d, now_mono_us);
    if (off != d->anchor_offset_s) {
        sched_due_reanchor_mono(d,
                                sched_due_project_wall_utc(d, now_mono_us),
                                now_mono_us);
    }

    const sched_program_t *prog = d->prog;
    int64_t L = local_at_mono(d, now_mono_us);
    uint32_t mask = 0;

    for (int j = 0; j < prog->job_count; j++) {
        const sched_job_t *job = &prog->jobs[j];
        sched_due_job_t *dj = &d->jobs[j];
        uint32_t walk_budget = SCHED_MISSED_WALK_CAP;
        bool gate = gate_open_at(job, L);
        bool fired = false;

        if (job->has_window && job->on_enter && gate && !dj->gate_open) fired = true;
        dj->gate_open = gate ? 1 : 0;
        if (dj->boot_pending && gate) {
            fired = true;
            dj->boot_pending = 0;
        }

        for (int i = 0; i < job->trigger_count; i++) {
            const sched_trigger_t *tr = &job->triggers[i];
            int64_t D_us = dj->due_us[i];
            int64_t D_local = dj->wall_due_local[i];

            if (D_us == SCHED_DUE_NONE_US && trigger_can_resolve(tr)) {
                D_local = next_trigger_due(tr, L);
                set_wall_due(d, dj, i, D_local, now_mono_us);
                D_us = dj->due_us[i];
            }

            const int64_t grace_us =
                (tr->kind == SCHED_TRIG_EVERY
                     ? tr->u.every.period_ms / 1000
                     : LATE_GRACE_DEFAULT_S) * US_PER_S;

            while (D_us != SCHED_DUE_NONE_US && D_us <= now_mono_us) {
                uint64_t lateness_us =
                    (uint64_t)now_mono_us - (uint64_t)D_us;
                if (lateness_us > (uint64_t)grace_us) {
                    uint32_t n = 0;
                    bool any_open = false, saturated = false;

                    if (tr->kind == SCHED_TRIG_EVERY) {
                        const int64_t period_us = tr->u.every.period_ms * 1000;
                        const int64_t limit_us = now_mono_us - grace_us;
                        if (!job->has_window) {
                            uint64_t distance =
                                (uint64_t)limit_us - (uint64_t)D_us;
                            uint64_t slots =
                                distance / (uint64_t)period_us + 1u;
                            if (job->missed == SCHED_MISSED_RUN_ONCE) {
                                any_open = slots != 0;
                            } else {
                                accumulate_skipped(dj, slots);
                            }
                            D_us = first_every_after(D_us, limit_us, period_us);
                        } else {
                            D_us = walk_every_missed(d, tr, job, D_us, limit_us,
                                                     &walk_budget, &n, &any_open,
                                                     &saturated);
                            if (job->missed != SCHED_MISSED_RUN_ONCE) {
                                accumulate_skipped(dj, n);
                            }
                        }
                        dj->due_us[i] = D_us;
                        sync_every_wall_due(d, dj, i, now_mono_us);
                        D_local = dj->wall_due_local[i];
                    } else {
                        int64_t limit_local =
                            local_at_mono(d, now_mono_us - grace_us);
                        int64_t before = D_local;
                        D_local = walk_wall_missed(tr, job, D_local, limit_local,
                                                   &walk_budget, &n, &any_open,
                                                   &saturated);
                        if (D_local == before) {
                            /* Spring-forward makes the local clock at the
                             * monotonic grace horizon earlier than a scheduled
                             * nonexistent wall instant (01:50 < 02:30), even
                             * though that instant is already 30 min late in
                             * monotonic time. Consume exactly that one slot so
                             * the loop always advances; this is the existing
                             * DST contract's counted spring-gap skip. */
                            if (gate_open_at(job, before)) {
                                n++;
                                any_open = true;
                            }
                            D_local = next_trigger_due(tr, before);
                        }
                        set_wall_due(d, dj, i, D_local, now_mono_us);
                        D_us = dj->due_us[i];
                        if (job->missed != SCHED_MISSED_RUN_ONCE) {
                            accumulate_skipped(dj, n);
                        }
                    }
                    if (saturated) dj->skipped_saturated = 1;
                    if (job->missed == SCHED_MISSED_RUN_ONCE && any_open) fired = true;
                    continue;
                }

                if (!gate) {
                    if (tr->kind == SCHED_TRIG_EVERY) {
                        int64_t period_us = tr->u.every.period_ms * 1000;
                        D_us = first_every_after(D_us, now_mono_us, period_us);
                        dj->due_us[i] = D_us;
                        sync_every_wall_due(d, dj, i, now_mono_us);
                    } else {
                        D_local = next_trigger_due(tr, L);
                        set_wall_due(d, dj, i, D_local, now_mono_us);
                    }
                    break;
                }

                if (tr->kind == SCHED_TRIG_CRON) {
                    if (dj->fired_minute == L / 60) {
                        D_local = next_trigger_due(tr, L);
                        set_wall_due(d, dj, i, D_local, now_mono_us);
                        break;
                    }
                    dj->fired_minute = L / 60;
                }

                fired = true;
                if (tr->kind == SCHED_TRIG_EVERY) {
                    int64_t period_us = tr->u.every.period_ms * 1000;
                    dj->due_us[i] =
                        (D_us >= 0 && INT64_MAX - D_us < period_us)
                            ? INT64_MAX : D_us + period_us;
                    sync_every_wall_due(d, dj, i, now_mono_us);
                } else {
                    D_local = next_trigger_due(tr, L > D_local ? L : D_local);
                    set_wall_due(d, dj, i, D_local, now_mono_us);
                }
                break;
            }
        }

        if (fired) {
            mask |= 1u << j;
            dj->runs++;
        }
    }
    (void)local_at_mono(d, now_mono_us); /* restore current sun offset */
    return mask;
}

uint32_t sched_due_poll(sched_due_t *d, int64_t now_utc)
{
    return sched_due_poll_mono(d, now_utc * US_PER_S);
}

int64_t sched_due_next_mono(const sched_due_t *d, int64_t now_mono_us)
{
    const sched_program_t *prog = d->prog;
    int64_t L = local_at_mono(d, now_mono_us);
    int64_t best = -1;
    for (int j = 0; j < prog->job_count; j++) {
        const sched_job_t *job = &prog->jobs[j];
        const sched_due_job_t *dj = &d->jobs[j];
        bool gate = gate_open_at(job, L);
        int64_t cand = SCHED_DUE_NONE_US;
        if (gate) {
            if (dj->boot_pending) cand = now_mono_us;
            for (int i = 0; i < job->trigger_count; i++) {
                int64_t due_us = dj->due_us[i];
                if (due_us == SCHED_DUE_NONE_US) continue;
                int64_t c = due_us <= now_mono_us ? now_mono_us : due_us;
                if (cand == SCHED_DUE_NONE_US || c < cand) cand = c;
            }
        } else if (job->has_window) {
            int64_t open_local;
            if (sched_window_next_open(&job->window, L, &open_local)) {
                cand = project_local(d, open_local, now_mono_us);
                if (cand < now_mono_us) cand = now_mono_us;
            }
        }
        if (cand != SCHED_DUE_NONE_US && (best < 0 || cand < best)) best = cand;
    }
    (void)local_at_mono(d, now_mono_us);
    return best;
}

int64_t sched_due_next(const sched_due_t *d, int64_t now_utc)
{
    int64_t now_mono_us = now_utc * US_PER_S;
    int64_t next_us = sched_due_next_mono(d, now_mono_us);
    return next_us < 0 ? -1 : local_at_mono(d, next_us);
}
