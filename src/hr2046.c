#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "lua.h"
#include "lauxlib.h"

#include "hr2046.h"

#define HR2046_SCLK        GPIO_NUM_41
#define HR2046_MOSI        GPIO_NUM_2
#define HR2046_MISO        GPIO_NUM_1
#define HR2046_CS          GPIO_NUM_42

/* ครึ่งคาบของ clock (µs) 2 => ประมาณ 250 kHz ปลอดภัยสำหรับ XPT2046/HR2046 */
#define HR2046_HALF_PERIOD_US   2


/* ---------------------------------------------------------
 * GPIO setup (bit-bang SPI mode 0)
 * --------------------------------------------------------- */

esp_err_t hr2046_init(void)
{
    esp_err_t ret;

    /* ปล่อยขาจาก peripheral อื่นที่อาจจองไว้ก่อน */
    gpio_reset_pin(HR2046_SCLK);
    gpio_reset_pin(HR2046_MOSI);
    gpio_reset_pin(HR2046_MISO);
    gpio_reset_pin(HR2046_CS);

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << HR2046_SCLK) |
                        (1ULL << HR2046_MOSI) |
                        (1ULL << HR2046_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&out_cfg);
    if (ret != ESP_OK)
        return ret;

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << HR2046_MISO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&in_cfg);
    if (ret != ESP_OK)
        return ret;

    /* สถานะเริ่มต้น: CS สูง, SCLK ต่ำ (mode 0) */
    gpio_set_level(HR2046_CS, 1);
    gpio_set_level(HR2046_SCLK, 0);
    gpio_set_level(HR2046_MOSI, 0);

    return ESP_OK;
}


/* ---------------------------------------------------------
 * Bit-bang: ส่ง/รับ 1 ไบต์ (MSB first, mode 0)
 * --------------------------------------------------------- */

static uint8_t hr2046_xfer_byte(uint8_t out)
{
    uint8_t in = 0;

    for (int i = 7; i >= 0; i--) {
        gpio_set_level(HR2046_MOSI, (out >> i) & 1);
        esp_rom_delay_us(HR2046_HALF_PERIOD_US);

        gpio_set_level(HR2046_SCLK, 1);          /* ขอบขึ้น: ชิปอ่าน MOSI */
        esp_rom_delay_us(HR2046_HALF_PERIOD_US);

        in = (uint8_t)((in << 1) | (gpio_get_level(HR2046_MISO) & 1));

        gpio_set_level(HR2046_SCLK, 0);          /* ขอบลง: ชิปเลื่อนบิตถัดไป */
    }

    return in;
}


/* ---------------------------------------------------------
 * Read one 12-bit ADC value
 * --------------------------------------------------------- */

static uint16_t hr2046_read_raw(uint8_t command)
{
    uint8_t rx[3];

    gpio_set_level(HR2046_CS, 0);
    esp_rom_delay_us(HR2046_HALF_PERIOD_US);

    rx[0] = hr2046_xfer_byte(command);   /* ส่งคำสั่ง (ข้อมูลที่ได้ทิ้ง) */
    rx[1] = hr2046_xfer_byte(0x00);
    rx[2] = hr2046_xfer_byte(0x00);

    esp_rom_delay_us(HR2046_HALF_PERIOD_US);
    gpio_set_level(HR2046_CS, 1);

    /*
     * ข้อมูล 12 บิตอยู่ใน rx[1], rx[2]
     *   [rx1: D11..D4][rx2: D3..D0 xxxx]
     * ต้องเลื่อนขวา 3 เพราะมีบิต busy ตามหลังคำสั่ง
     */
    uint16_t value = ((uint16_t)rx[1] << 8) | rx[2];
    value >>= 3;

    return value & 0x0FFF;
}


/* ---------------------------------------------------------
 * Public functions
 * --------------------------------------------------------- */

uint16_t hr2046_read_x(void)
{
    return hr2046_read_raw(0xD0);
}

uint16_t hr2046_read_y(void)
{
    return hr2046_read_raw(0x90);
}

uint16_t hr2046_read_z1(void)
{
    return hr2046_read_raw(0xB0);
}

uint16_t hr2046_read_z2(void)
{
    return hr2046_read_raw(0xC0);
}