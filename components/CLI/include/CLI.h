#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cli_start(void);

/* Boot-resolved MQTT identity, after fallback and {MAC} substitution. The
 * caller owns this immutable string for the lifetime of the console. */
void cli_set_thing_name(const char *thing_name);

#ifdef __cplusplus
}
#endif
