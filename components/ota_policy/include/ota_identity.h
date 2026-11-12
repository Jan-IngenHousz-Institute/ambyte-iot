#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Bounded additive JSON members, without outer braces; no I/O or allocation.
 * signed describes this running build's enforced IDF startup/update policy.
 * Older builds without these fields are unsigned for fleet targeting. */
bool ota_running_identity_json(char *out, size_t capacity);
