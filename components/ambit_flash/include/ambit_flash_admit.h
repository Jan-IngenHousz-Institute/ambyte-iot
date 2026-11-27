#pragma once
/* Maintenance admission around a flash (PR #112 round 3, HAR-310).
 *
 * Every AMBIT flash must run inside the global maintenance admission
 * (main/app_main.c app_maintenance_begin/end over maint_gate.h): while it is
 * held, charger re-detect pulses and AICO re-arms are vetoed, and it cannot be
 * granted while the MP2731 driver holds a charger quarantine (a BG_EN or
 * USB_DET_EN not proven back at 0), so no charger bit recovery can run during
 * the flash. Pure: host-tested in tests/charger_gate_host.c with the real
 * gate and the production MP2731 driver. */
#include <stdbool.h>

typedef struct {
    bool (*begin)(void);   /* may wait (bounded); false = refused, nothing held */
    void (*end)(void);
} ambit_flash_maint_t;

/* The flash did not run: no gate wired, or the admission was refused. */
#define AMBIT_FLASH_ADMIT_DECLINED (-2)

/* Runs flash(ctx) only after m->begin() granted, and calls m->end() exactly
 * once after it returns, whatever it returns. Returns flash's result, or
 * AMBIT_FLASH_ADMIT_DECLINED without calling flash. A NULL m, begin or end
 * is refused (fail closed): an unwired flash path never flashes. */
int ambit_flash_admitted(const ambit_flash_maint_t *m, int (*flash)(void *ctx), void *ctx);
