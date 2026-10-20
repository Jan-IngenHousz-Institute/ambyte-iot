#pragma once
#include "../include/esp_heap_caps.h"
#define MALLOC_CAP_DEFAULT 0
void *heap_caps_calloc(size_t,size_t,unsigned);
void heap_caps_free(void *);
