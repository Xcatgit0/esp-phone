#pragma once
#include "FreeRTOS.h"
typedef struct {
    TaskHandle_t  xHandle;
    const char   *pcTaskName;
    UBaseType_t   uxCurrentPriority;
    unsigned long usStackHighWaterMark;
    unsigned long ulRunTimeCounter;
} TaskStatus_t;
#ifdef __cplusplus
extern "C" {
#endif
void         vTaskDelay(TickType_t ticks);
TickType_t   xTaskGetTickCount(void);
UBaseType_t  uxTaskGetNumberOfTasks(void);
UBaseType_t  uxTaskGetSystemState(TaskStatus_t *arr, UBaseType_t len, uint32_t *total_runtime);
BaseType_t   xTaskGetCoreID(TaskHandle_t h);
typedef void (*TaskFunction_t)(void *);
BaseType_t   xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                     UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
void         vTaskDelete(TaskHandle_t h);
#ifdef __cplusplus
}
#endif
