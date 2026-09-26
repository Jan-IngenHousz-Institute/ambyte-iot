#pragma once

/* On-device SD-overflow verification CLI (CONFIG_AMBYTE_EVQ_HIL only; see
 * docs/evq-sd-overflow-hil-contract.md). app_main wires it; a release image
 * compiles none of it. */

#include "sdkconfig.h"

#if CONFIG_AMBYTE_EVQ_HIL

#ifdef __cplusplus
extern "C" {
#endif

/* The production low-battery SD park/unpark routines in app_main, so
 * `evq_hil sd_park|sd_unpark` exercises exactly that path (M-1). */
void evq_hil_set_park_hooks(void (*park)(void), void (*unpark)(void));

/* Register the `evq_hil` console command (after the console exists). */
void evq_hil_register(void);

#ifdef __cplusplus
}
#endif

#endif
