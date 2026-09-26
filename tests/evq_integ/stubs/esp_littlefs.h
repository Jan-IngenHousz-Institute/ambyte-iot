#pragma once
#include <stddef.h>
#include "esp_err.h"
/* total = configured flash capacity; used = block-rounded bytes under EVSTORE_MOUNT. */
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes);
