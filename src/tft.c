#include "tft.h"
#include "cursor.h"
#include <stdio.h>
#include "log.h"
#include "usb/usb_host.h"
QueueHandle_t pushQueue;
esp_lcd_panel_handle_t panel = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;

/*
 * Framebuffer อยู่ใน PSRAM
 *
 * 240 × 320 × 2 = 153600 bytes
 */
uint16_t *framebuffer = NULL;

#define LCD_FB_SIZE (LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t))

/* --------------------------------------------------
 * RGB565
 *
 * RRRRR GGGGGG BBBBB
 *
 * กลับ bit + swap G/B
 * -------------------------------------------------- */

/* --------------------------------------------------
 * INIT
 * -------------------------------------------------- */
SemaphoreHandle_t push_mutex;
esp_err_t tft_init(void)
{    
    push_mutex = xSemaphoreCreateMutex();
    xSemaphoreGive(push_mutex);
    pushQueue = xQueueCreate(1,sizeof(uint8_t));
    /* ---------------------------------------------
     * Allocate framebuffer ใน PSRAM
     * --------------------------------------------- */
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    LOG_INFO("PSRAM total: %u bytes", (unsigned)psram_total);
    LOG_INFO("PSRAM free : %u bytes", (unsigned)psram_free);
    framebuffer = (uint16_t *)heap_caps_malloc(
        LCD_FB_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA |MALLOC_CAP_8BIT);

    if (framebuffer == NULL)
    {
        LOG_ERROR("ERROR: Cannot allocate framebuffer in PSRAM");
    }
    else
    {
        LOG_INFO("Framebuffer allocated: %p", framebuffer);
        LOG_INFO("Size: %u bytes\n", LCD_FB_SIZE);
    }

    LOG_INFO(
        "TFT framebuffer: %d bytes in PSRAM",
        LCD_FB_SIZE);

    /* ---------------------------------------------
     * SPI BUS
     * --------------------------------------------- */

    spi_bus_config_t buscfg = {
        .sclk_io_num = 7,
        .mosi_io_num = 15,
        .miso_io_num = -1,

        .quadwp_io_num = -1,
        .quadhd_io_num = -1,

        .max_transfer_sz = LCD_FB_SIZE,
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(
            SPI2_HOST,
            &buscfg,
            SPI_DMA_CH_AUTO));

    /* ---------------------------------------------
     * LCD IO
     * --------------------------------------------- */

    

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = 17,
        .cs_gpio_num = 18,

        .pclk_hz = 80 * 1000 * 1000,

        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,

        .spi_mode = 0,

        .trans_queue_depth = 10,
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)SPI2_HOST,
            &io_config,
            &io_handle));

    /* ---------------------------------------------
     * ST7789
     * --------------------------------------------- */

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = 16,

        .bits_per_pixel = 16,

        .rgb_ele_order =
            LCD_RGB_ELEMENT_ORDER_RGB,
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_st7789(
            io_handle,
            &panel_config,
            &panel));

    /* ---------------------------------------------
     * START DISPLAY
     * --------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_lcd_panel_reset(panel));

    ESP_ERROR_CHECK(
        esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x21, NULL, 0)); // INVON
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xB0, (uint8_t[]){0x00,0xE8}, 2));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 35));
    ESP_ERROR_CHECK(
        esp_lcd_panel_disp_on_off(
            panel,
            true));

    /* ---------------------------------------------
     * Clear framebuffer
     * --------------------------------------------- */

    tft_fill(0x0000);

    tft_push();

    return ESP_OK;
}

/* --------------------------------------------------
 * Fill framebuffer
 * -------------------------------------------------- */

void tft_fill(uint16_t color)
{
    
    if (framebuffer == NULL)
        return;

    uint16_t converted =
        tft_convert_color(color);

    for (int i = 0;
         i < LCD_WIDTH * LCD_HEIGHT;
         i++)
    {
        framebuffer[i] = converted;
    }
}

/* --------------------------------------------------
 * Draw pixel
 * -------------------------------------------------- */

void tft_draw_pixel(
    int x,
    int y,
    uint16_t color)
{
    if (framebuffer == NULL)
        return;

    if (x < 0 || x >= LCD_WIDTH)
        return;

    if (y < 0 || y >= LCD_HEIGHT)
        return;

    framebuffer[y * LCD_WIDTH + x] = tft_convert_color(color);
}

/* --------------------------------------------------
 * Send framebuffer → TFT
 * -------------------------------------------------- */
uint8_t event = 1;
void tft_push(void)
{
    if (xSemaphoreTake(push_mutex, portMAX_DELAY) == pdTRUE) {
    // ได้ lock แล้ว
    if(trans_done_sem!=NULL)xSemaphoreTake(trans_done_sem, portMAX_DELAY);   
    //LOG_INFO("PUSH");
    ESP_ERROR_CHECK(
        esp_lcd_panel_draw_bitmap(
            panel,

            0,
            0,

            LCD_WIDTH,
            LCD_HEIGHT,

            framebuffer));
        }
    xSemaphoreGive(push_mutex);
    if(drawCursorQueue!=NULL){xQueueOverwrite(drawCursorQueue,&event);};
        //draw_cursor();
        return;
}

/* --------------------------------------------------
 * TEST
 * -------------------------------------------------- */

void tft_test(void)
{
    const uint16_t colors[] = {
        0xF800,
        0x07E0,
        0x001F,
        0xFFFF,
        0x0000};

    const char *names[] = {
        "RED",
        "GREEN",
        "BLUE",
        "WHITE",
        "BLACK"};

    for (int c = 0; c < 5; c++)
    {
        printf(
            "TEST COLOR: %s\n",
            names[c]);

        tft_fill(colors[c]);

        tft_push();

        vTaskDelay(
            pdMS_TO_TICKS(2000));
    }
}