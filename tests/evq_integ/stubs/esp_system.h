#pragma once
#include <stdint.h>
/* A production self-reboot is a test failure in this harness (esp_stubs.c). */
void esp_restart(void) __attribute__((noreturn));
uint32_t esp_get_free_heap_size(void);
