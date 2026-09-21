#include "esp_adc/adc_oneshot.h"
#include "tft.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "cursor.h"
SemaphoreHandle_t trans_done_sem;
QueueHandle_t drawCursorQueue;
static uint16_t *mouse_box = NULL;
#define JOY_X_CHANNEL ADC_CHANNEL_2 // GPIO3
#define JOY_Y_CHANNEL ADC_CHANNEL_7 // GPIO8
#include "string.h"
int cursor_x = 0;
int cursor_y = 0;
uint8_t isSHOW;
void cursor_task(void *args);
static adc_oneshot_unit_handle_t adc_handle;
#define bufferSize 512
uint8_t eventDraw;
void queue_task(void *arg) {
    while(1){
        if(xQueueReceive(pushQueue,&eventDraw,16)==pdPASS) tft_push();
        if(xQueueReceive(drawCursorQueue,&eventDraw,16)==pdPASS) draw_cursor();
    }
}
static bool on_color_trans_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata,
    void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;

    if (trans_done_sem != NULL) {
        xSemaphoreGiveFromISR(trans_done_sem, &high_task_wakeup);
    }

    return high_task_wakeup == pdTRUE;
}
void joystick_init(void)
{
    eventDraw = 0;
    drawCursorQueue = xQueueCreate(1,sizeof(uint8_t));
    isSHOW = 0;
    mouse_box = (uint16_t *)heap_caps_malloc(bufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (mouse_box == NULL)
    {
        printf("ERROR: mouse_box allocation FAILED!\n");
        abort();
    }
    trans_done_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(trans_done_sem); // ตั้งค่าเริ่มต้นว่า "ว่าง" พร้อมส่งได้

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    // io_handle คือ handle ตัวเดียวกับที่ใช้ตอน esp_lcd_new_panel_io_spi(...)
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, NULL));
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };

    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(adc_handle, JOY_X_CHANNEL, &cfg));

    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(adc_handle, JOY_Y_CHANNEL, &cfg));
    xTaskCreatePinnedToCore(cursor_task, "cursor", 2048, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(queue_task, "TFTQueue", 2048, NULL, 6, NULL, 1);
}

void cursor_task(void *arg)
{
    
    int x, y;
    x=0;
    y=0;
    int oldX = 0, oldY = 0;
    while (1)
    {


        
        adc_oneshot_read(adc_handle, JOY_X_CHANNEL, &x);
        adc_oneshot_read(adc_handle, JOY_Y_CHANNEL, &y);

        if (x > 2800)
            cursor_x--;
        else if (x < 1200)
            cursor_x++;

        if (y > 2800)
            cursor_y++;
        else if (y < 1200)
            cursor_y--;
        if (!(oldX == cursor_x && oldY == cursor_y))
        {
            draw_cursor();
        }
        if (cursor_x < 0)
            cursor_x = 0;
        if (cursor_x > LCD_WIDTH)
            cursor_x = LCD_WIDTH;
        if (cursor_y < 0)            cursor_y = 0;
        if (cursor_y > LCD_HEIGHT)
            cursor_y = LCD_HEIGHT;
        oldX = cursor_x;
        oldY = cursor_y;
        vTaskDelay(pdMS_TO_TICKS(16));

    }
}

const uint16_t *cursor_bitmap = cursor_window;
static uint16_t *restore_box = NULL;   // เก็บพื้นหลังเดิมตำแหน่งก่อนหน้า
static int old_x = -1, old_y = -1;

void draw_cursor(void)
{  
    
    if (xSemaphoreTake(push_mutex, portMAX_DELAY) == pdTRUE) {
    if (mouse_box == NULL || panel == NULL || framebuffer == NULL) {
        xSemaphoreGive(push_mutex);
        return;
    }
    
    if (restore_box == NULL) {
        restore_box = heap_caps_malloc(bufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (restore_box == NULL) return;
    }
     xSemaphoreTake(trans_done_sem, portMAX_DELAY);
    // 1) คืนค่าพื้นหลังเดิมที่ตำแหน่งเก่า (ถ้ามี)
    for (int y = 0; y < 16; y++) {
        memcpy(&restore_box[y*16],
               &framebuffer[(old_y+y)*LCD_WIDTH+old_x],
               16 * sizeof(uint16_t));
        }

    // 2) เก็บพื้นหลัง ณ ตำแหน่งใหม่ไว้สำหรับรอบถัดไป (ก่อนโดน invert >>>>>>>>>>> deprecated
   
    
     esp_lcd_panel_draw_bitmap(
            panel, old_x, old_y, old_x + 16, old_y + 16, restore_box);
    xSemaphoreTake(trans_done_sem, portMAX_DELAY);
    for (int y = 0; y < 16; y++) {
        memcpy(&mouse_box[y*16],
               &framebuffer[(cursor_y+y)*LCD_WIDTH+cursor_x],
               16 * sizeof(uint16_t));
        }
    // 3) invert เฉพาะ pixel รูป cursor แล้ววาดตำแหน่งใหม่
    isSHOW = 1;
    for (int y = 0; y < 16; y++) {
        uint16_t bitmap_row = cursor_bitmap[y];
        for (int x = 0; x < 16; x++) {
            if (bitmap_row & (0x8000 >> x))
                mouse_box[y*16+x] = ~mouse_box[y*16+x];
        }
    }
    esp_lcd_panel_draw_bitmap(
        panel, cursor_x, cursor_y, cursor_x + 16, cursor_y + 16, mouse_box);
    
    old_x = cursor_x;
    old_y = cursor_y;
    xSemaphoreGive(push_mutex);
    }
}