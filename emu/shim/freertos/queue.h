#pragma once
#include "FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
QueueHandle_t emu_queue_create(unsigned length, unsigned item_size);
BaseType_t    emu_queue_overwrite(QueueHandle_t q, const void *item);
BaseType_t    emu_queue_send(QueueHandle_t q, const void *item);
BaseType_t    emu_queue_send_isr(QueueHandle_t q, const void *item, BaseType_t *woken);
BaseType_t    emu_queue_receive(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t    emu_queue_reset(QueueHandle_t q);
#ifdef __cplusplus
}
#endif
#define xQueueCreate(len, size)             emu_queue_create((len), (size))
#define xQueueOverwrite(q, item)            emu_queue_overwrite((q), (item))
#define xQueueSend(q, item, ticks)          emu_queue_send((q), (item))
#define xQueueSendFromISR(q, item, woken)   emu_queue_send_isr((q), (item), (woken))
#define xQueueReceive(q, item, ticks)       emu_queue_receive((q), (item), (ticks))
#define xQueueReset(q)                      emu_queue_reset((q))
