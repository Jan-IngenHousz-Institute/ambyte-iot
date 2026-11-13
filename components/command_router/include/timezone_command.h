#pragma once
#include <stdbool.h>
#include "cJSON.h"

/* The shared worker owns persistence + apply. MQTT only copies bounded input;
 * it never blocks on NVS, libc timezone locks or another maintenance operation. */
typedef struct {
    void (*reply)(const char *json);
    bool (*submit)(void (*run)(void *), void *arg);
    bool (*maintenance_begin)(void);
    void (*maintenance_end)(void);
} timezone_command_config_t;

void timezone_command_init(const timezone_command_config_t *config);
/* Returns false for other commands. Caller still owns root. */
bool timezone_command_handle(const cJSON *root);
