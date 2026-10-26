#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct { uint32_t image_len; } esp_image_metadata_t;
#define ESP_IMAGE_VERIFY 0
esp_err_t esp_image_verify(int, const esp_partition_pos_t *, esp_image_metadata_t *);
