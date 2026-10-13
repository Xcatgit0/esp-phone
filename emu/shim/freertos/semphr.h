#pragma once
#include "queue.h"
#ifdef __cplusplus
extern "C" {
#endif
SemaphoreHandle_t emu_mutex_create(void);
BaseType_t        emu_mutex_take(SemaphoreHandle_t m, TickType_t ticks);
BaseType_t        emu_mutex_give(SemaphoreHandle_t m);
#ifdef __cplusplus
}
#endif
#define xSemaphoreCreateMutex()        emu_mutex_create()
#define xSemaphoreTake(s, ticks)       emu_mutex_take((s), (ticks))
#define xSemaphoreGive(s)              emu_mutex_give((s))
