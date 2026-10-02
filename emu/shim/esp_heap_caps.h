#pragma once
#include <stdint.h>
#include <stddef.h>
#define MALLOC_CAP_EXEC     (1u << 0)
#define MALLOC_CAP_32BIT    (1u << 1)
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_DMA      (1u << 3)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_DEFAULT  (1u << 12)
#ifdef __cplusplus
extern "C" {
#endif
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
void  *heap_caps_malloc(size_t size, uint32_t caps);
#ifdef __cplusplus
}
#endif
