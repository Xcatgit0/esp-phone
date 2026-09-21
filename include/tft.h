#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"

#define LCD_WIDTH  320
#define LCD_HEIGHT 170

esp_err_t tft_init(void);

void tft_fill(uint16_t color);
void tft_draw_pixel(int x, int y, uint16_t color);
void tft_push(void);

void tft_test(void);
extern uint16_t *framebuffer;
extern esp_lcd_panel_handle_t panel;
extern QueueHandle_t pushQueue;
extern SemaphoreHandle_t push_mutex;
static inline uint16_t tft_convert_color(uint16_t color)
{
return color;
}