/* Minimal FreeRTOS shim for the Lua-facing code of the phone project. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define configTICK_RATE_HZ   100            /* sdkconfig: CONFIG_FREERTOS_HZ=100 */
#define portTICK_PERIOD_MS   ((TickType_t)(1000 / configTICK_RATE_HZ))
#define pdMS_TO_TICKS(ms)    ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000))
#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define pdFAIL   0
#define portMAX_DELAY 0xFFFFFFFFu
#define errQUEUE_FULL 0
#define portYIELD_FROM_ISR(...) ((void)0)
typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef void    *QueueHandle_t;
typedef void    *SemaphoreHandle_t;
typedef void    *TaskHandle_t;
#ifdef __cplusplus
extern "C" {
#endif
void *pvPortMalloc(size_t size);
void  vPortFree(void *p);
#ifdef __cplusplus
}
#endif
