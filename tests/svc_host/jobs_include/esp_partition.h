#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef struct { unsigned address, size; } esp_partition_t;
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
