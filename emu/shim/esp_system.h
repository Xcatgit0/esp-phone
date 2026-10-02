#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void esp_restart(void) __attribute__((noreturn));
uint32_t esp_get_free_heap_size(void);
#ifdef __cplusplus
}
#endif
