#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCRIPT_IDENTITY_SHA256_LEN 65
#define SCRIPT_IDENTITY_VERSION_LEN 32

/* Exact active-file identity plus optional release provenance. The SHA is always
 * derived from /sdcard/main.lua. Release fields are returned only when their
 * persisted digest still matches those bytes, so a manual SD-card replacement
 * cannot inherit stale version metadata. */
typedef struct {
    char sha256[SCRIPT_IDENTITY_SHA256_LEN];
    char version[SCRIPT_IDENTITY_VERSION_LEN];
    char built_against_fw[SCRIPT_IDENTITY_VERSION_LEN];
    char installed_on_fw[SCRIPT_IDENTITY_VERSION_LEN];
    bool release_metadata_verified;
} script_identity_t;

typedef esp_err_t (*script_identity_read_fn)(script_identity_t *out);

#ifdef __cplusplus
}
#endif
