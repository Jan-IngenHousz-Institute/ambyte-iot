#ifndef EVQ_HOST_LITTLEFS_H
#define EVQ_HOST_LITTLEFS_H
#include <stddef.h>
#include "esp_err.h"
/* Capacity/usage come from the media shim (fsshim.c), so ENOSPC and the
 * pressure watermark see the same bytes. */
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes);
#endif
