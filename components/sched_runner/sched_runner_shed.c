/*
 * sched_runner_shed.c — which jobs brownout protection sheds (rule and field
 * origin: sched_runner_priv.h). Pure: reads only the compiled program, so
 * tests/sched_runner_host.c runs it against the real default.yaml and
 * qe_multichannel.yaml compiles.
 */
#include <string.h>

#include "sched_runner_priv.h"

static const sched_entry_t *entry(const sched_program_t *prog, const sched_step_t *step,
                                  const char *name)
{
    for (int e = 0; e < step->entry_count; e++) {
        const sched_entry_t *en = &prog->entries[step->entry_start + e];
        if (strcmp(step->action->inputs[en->input_idx].name, name) == 0) return en;
    }
    return NULL;
}

bool sched_step_is_actinic(const sched_program_t *prog, const sched_step_t *step)
{
    if (prog == NULL || step == NULL || step->action == NULL) return false;
    const char *action = step->action->name;
    if (strcmp(action, "ambit/actinic") == 0) {
        const sched_entry_t *lv = entry(prog, step, "level");
        return lv == NULL || lv->u.i != 0;   /* required input; absent = unsafe */
    }
    if (strcmp(action, "ambit/trace") != 0) return false;
    const sched_entry_t *pe = entry(prog, step, "protocol");
    const char *pname = (pe != NULL && pe->type == SCHED_VAL_STR) ? sched_pool_str(prog, pe->u.str_off) : NULL;
    if (pname == NULL) return true;
    for (int i = 0; i < prog->protocol_count; i++) {
        const char *n = sched_pool_str(prog, prog->protocols[i].name_off);
        if (n == NULL || strcmp(n, pname) != 0) continue;
        const sched_protocol_t *pr = &prog->protocols[i];
        for (int s = 0; s < pr->segment_count; s++) {
            if (pr->segments[s].actinic != 0) return true;
        }
        return false;
    }
    return true;   /* unknown protocol: the compiler forbids it; stay safe */
}

sched_shed_t sched_job_shed(const sched_program_t *prog, const sched_job_t *job,
                            bool block_flash, bool block_ambit)
{
    if (prog == NULL || job == NULL || (!block_flash && !block_ambit)) return SCHED_SHED_NONE;
    bool flash = false, ambit = false;
    for (int s = 0; s < job->step_count; s++) {
        const sched_step_t *step = &job->steps[s];
        if (step->action == NULL) continue;
        if (strncmp(step->action->name, "ambit/", 6) == 0) ambit = true;
        if (sched_step_is_actinic(prog, step)) flash = true;
    }
    /* hold implies no_flash: a flash job is counted as a flash skip in both. */
    if (flash && (block_flash || block_ambit)) return SCHED_SHED_FLASH;
    if (ambit && block_ambit) return SCHED_SHED_AMBIT;
    return SCHED_SHED_NONE;
}
