#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_AMBYTE_BENCH_DIAG
/** Start the once-per-minute, low-priority root-cause bench reporter. */
esp_err_t bench_diag_start(void);
#endif

#ifdef __cplusplus
}
#endif
