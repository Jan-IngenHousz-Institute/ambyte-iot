#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool command_router_pong_json(char *out, size_t capacity, const char *id,
                              const char *device_id, const char *fw, int64_t uptime_ms);
