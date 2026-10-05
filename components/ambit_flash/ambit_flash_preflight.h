#pragma once

/* SD preflight for the AMBIT ROM flash (2026-09 write-integrity audit).
 *
 * The recovery-directory mkdirs and the four region-size probes used to run with
 * no sdcard_io_begin/end: the hot-plug monitor's teardown could free the FAT
 * volume underneath them (the same use-after-free class audit R-6 closed for the
 * streaming path). They now run inside ONE io bracket that is released before
 * the caller takes the UART bus — no SD ref is ever held across flashing — and
 * any refusal or failure returns before the target AMBIT is touched.
 *
 * Pure C over stdio (+ sd_card gate, sd_diag); host-compiled by tests/ambit_host. */

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Size of dir/fname in bytes (>0), or 0 when missing/empty/unreadable.
 * Caller holds an SD io ref. */
long ambit_flash_region_size(const char *dir, const char *fname);

/* Create root and dir (EEXIST is fine), then require every fnames[i] in dir to
 * be present and non-empty. ESP_ERR_INVALID_STATE when the SD gate refuses,
 * ESP_FAIL when a directory cannot be created, ESP_ERR_NOT_FOUND when a region
 * file is missing/empty. */
esp_err_t ambit_flash_preflight(const char *root, const char *dir, const char *const *fnames, size_t n);

#ifdef __cplusplus
}
#endif
