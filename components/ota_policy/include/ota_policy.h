#pragma once
#include "esp_err.h"
#include "esp_partition.h"
#include "ota_manifest.h"
#include <stdio.h>

/* No boot selection; all update callers must admit before selecting a slot. */
esp_err_t ota_image_admit(const esp_partition_t *partition, size_t received,
                         char *reason, size_t capacity);
bool ota_policy_read_manifest(const esp_partition_t *partition, ota_manifest_t *out);
/* Diagnostic only. Call after OTA cleanup and workload/comms resume. */
bool ota_refusal_save(const char *id, const char *source, const char *reason);
void ota_policy_print(FILE *stream);
