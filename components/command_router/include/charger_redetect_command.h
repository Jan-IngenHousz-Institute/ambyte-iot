#pragma once

#include <stdbool.h>
#include "cJSON.h"

typedef struct {
    void (*reply)(const char *json);
    bool (*submit)(void (*run)(void *), void *arg);
} charger_redetect_command_config_t;

void charger_redetect_command_init(const charger_redetect_command_config_t *config);
/* `charger_redetect` {"method":"bg_en"|"dpdm"} and `charger_redetect_auto`
 * {"enabled":true|false, omitted = query}. Returns false for other commands.
 * Caller retains ownership of root; this handler copies bounded input and
 * leaves all charger work (I2C, the ~5 s settle reads) to the resident
 * maintenance worker. */
bool charger_redetect_command_handle(const cJSON *root);
