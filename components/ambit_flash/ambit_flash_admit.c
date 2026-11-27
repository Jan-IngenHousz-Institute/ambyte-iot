#include "ambit_flash_admit.h"

#include <stddef.h>

int ambit_flash_admitted(const ambit_flash_maint_t *m, int (*flash)(void *ctx), void *ctx)
{
    if (m == NULL || m->begin == NULL || m->end == NULL || flash == NULL) {
        return AMBIT_FLASH_ADMIT_DECLINED;
    }
    if (!m->begin()) return AMBIT_FLASH_ADMIT_DECLINED;
    const int result = flash(ctx);
    m->end();
    return result;
}
