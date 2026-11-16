#pragma once

#include <stdbool.h>
#include "cJSON.h"

typedef struct {
    void (*reply)(const char *json);
    bool (*submit)(void (*run)(void *), void *arg);
} iin_limit_command_config_t;

void iin_limit_command_init(const iin_limit_command_config_t *config);
/* Returns false for other commands. Caller retains ownership of root; this
 * handler copies bounded input and leaves I2C work to the resident worker. */
bool iin_limit_command_handle(const cJSON *root);
