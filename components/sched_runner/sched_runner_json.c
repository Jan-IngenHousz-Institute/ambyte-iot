/*
 * sched_runner_json.c — minimal JSON object writer for db/store-event's
 * compiler-validated flat maps: keys are schedule-authored strings, values
 * typed scalars with $-placeholders resolved through the injected callback
 * (design catalog row). Bounded: ≤16 keys per map, values out of the 4 KiB
 * string pool.
 *
 * Pure on purpose (no firmware deps): tests/sched_runner_host.c compiles this
 * file and drives it directly — including the NULL extra_val path that
 * reboot-looped a device when db/store-event ran without `with:` (T3 review
 * blocker 5). A schedule that compiles must never crash the runner.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sched_runner_priv.h"

void sched_jw_raw(sched_jw_t *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + (w->len < w->cap ? w->len : w->cap),
                      w->len < w->cap ? w->cap - w->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || w->len + (size_t)n >= w->cap) w->overflow = true;
    if (n > 0) w->len += (size_t)n;
}

void sched_jw_str(sched_jw_t *w, const char *s)
{
    if (s == NULL) { /* never deref NULL — belt-and-braces over caller checks */
        sched_jw_raw(w, "\"\"");
        return;
    }
    sched_jw_raw(w, "\"");
    for (const char *p = s; *p != '\0' && !w->overflow; p++) {
        if (*p == '"' || *p == '\\') sched_jw_raw(w, "\\%c", *p);
        else sched_jw_raw(w, "%c", *p);
    }
    sched_jw_raw(w, "\"");
}

bool sched_jw_job_placeholder(sched_jw_t *w, const char *ph,
                              const sched_runner_act_ctx_t *ctx)
{
    if (strcmp(ph, "$job.runs") == 0) {
        sched_jw_raw(w, "%lu", (unsigned long)ctx->runs);
    } else if (strcmp(ph, "$job.failures") == 0) {
        sched_jw_raw(w, "%lu", (unsigned long)ctx->failures);
    } else if (strcmp(ph, "$job.skipped") == 0) {
        /* Backward-compatible numeric lower bound. */
        sched_jw_raw(w, "%lu", (unsigned long)ctx->skipped);
    } else if (strcmp(ph, "$job.skipped_saturated") == 0) {
        sched_jw_raw(w, "%s", ctx->skipped_saturated ? "true" : "false");
    } else if (strcmp(ph, "$job.fail_streak") == 0) {
        sched_jw_raw(w, "%lu", (unsigned long)ctx->fail_streak);
    } else {
        return false;
    }
    return true;
}

static void jw_value(sched_jw_t *w, const sched_entry_t *en,
                     const sched_program_t *prog,
                     const sched_runner_act_ctx_t *ctx,
                     sched_jw_placeholder_fn placeholder)
{
    switch (en->type) {
    case SCHED_VAL_INT:   sched_jw_raw(w, "%lld", (long long)en->u.i); break;
    case SCHED_VAL_FLOAT: sched_jw_raw(w, "%g", en->u.f); break;
    case SCHED_VAL_BOOL:  sched_jw_raw(w, "%s", en->u.i ? "true" : "false"); break;
    default: {
        const char *s = sched_pool_str(prog, en->u.str_off);
        if (s != NULL && s[0] == '$' && placeholder != NULL) {
            placeholder(w, s, ctx);
        } else {
            sched_jw_str(w, s);
        }
        break;
    }
    }
}

bool sched_build_map_json(char *buf, size_t cap, const char *input_name,
                          const char *extra_key, const char *extra_val,
                          const sched_step_t *step, const sched_program_t *prog,
                          const sched_runner_act_ctx_t *ctx,
                          sched_jw_placeholder_fn placeholder)
{
    sched_jw_t w = { buf, cap, 0, false };
    sched_jw_raw(&w, "{");
    bool first = true;
    /* extra_val == NULL omits the member — the hardware crash was
     * jw_str(NULL) here when a store-event step carried no kind. */
    if (extra_key != NULL && extra_val != NULL) {
        sched_jw_str(&w, extra_key);
        sched_jw_raw(&w, ":");
        sched_jw_str(&w, extra_val);
        first = false;
    }
    for (int e = 0; e < step->entry_count; e++) {
        const sched_entry_t *en = &prog->entries[step->entry_start + e];
        const sched_input_decl_t *decl = &step->action->inputs[en->input_idx];
        if (strcmp(decl->name, input_name) != 0 || en->key_off == SCHED_POOL_NONE) {
            continue;
        }
        sched_jw_raw(&w, "%s", first ? "" : ",");
        sched_jw_str(&w, sched_pool_str(prog, en->key_off));
        sched_jw_raw(&w, ":");
        jw_value(&w, en, prog, ctx, placeholder);
        first = false;
    }
    sched_jw_raw(&w, "}");
    return !w.overflow;
}
