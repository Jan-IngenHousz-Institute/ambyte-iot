#ifndef EVQ_HOST_HEAP_CAPS_H
#define EVQ_HOST_HEAP_CAPS_H
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_DMA      (1u << 3)
size_t heap_caps_get_total_size(uint32_t caps);
#endif
