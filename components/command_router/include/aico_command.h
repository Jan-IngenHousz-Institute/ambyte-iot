#pragma once

#include <stdbool.h>
#include "cJSON.h"

typedef struct {
    void (*reply)(const char *json);
    bool (*submit)(void (*run)(void *), void *arg);
} aico_command_config_t;

void aico_command_init(const aico_command_config_t *config);
/* Returns false for other commands. Caller retains ownership of root; this
 * handler copies bounded input and leaves I2C work to the resident worker. */
bool aico_command_handle(const cJSON *root);
