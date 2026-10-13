/*
 * out_i2s_esp.c - I2S output backend for the DEVICE (ESP-IDF 6.x "i2s_std" driver) -> MAX98357(A) amplifier.
 *
 *   MAX98357   ESP32-S3
 *   BCLK   <-  GPIO5  (AUDIO_I2S_PIN_BCLK)
 *   LRC    <-  GPIO6  (AUDIO_I2S_PIN_LRC, word select)
 *   DIN    <-  GPIO4  (AUDIO_I2S_PIN_DOUT)
 *
 * 16-bit stereo Philips I2S, master. No MCLK (MAX98357 does not need one). The DMA ring is
 * I2S_DMA_DESC x I2S_DMA_FRAMES frames (~70 ms at 44.1 kHz): that is the jitter buffer between the audio task
 * and the hardware. On underrun the driver sends zeros (auto_clear), so a stalled network is silence, not noise.
 * Declaration: include/audio_out.h (shared with the emulator's backend).
 */
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "audio_out.h"

#define I2S_DMA_DESC 6
#define I2S_DMA_FRAMES 512
#define I2S_WRITE_TIMEOUT_MS 1000

static i2s_chan_handle_t s_tx = NULL;
static const int16_t s_zeros[I2S_DMA_FRAMES * 2] = {0};

esp_err_t audio_out_close(void)
{
    if (!s_tx)
        return ESP_OK;
    i2s_channel_disable(s_tx);
    esp_err_t e = i2s_del_channel(s_tx);
    s_tx = NULL;
    return e;
}

esp_err_t audio_out_open(int sample_rate)
{
    if (s_tx)
        audio_out_close();

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.dma_desc_num = I2S_DMA_DESC;
    cc.dma_frame_num = I2S_DMA_FRAMES;
    cc.auto_clear_after_cb = true; /* send zeros instead of repeating the last buffer when we under-run */
    esp_err_t e = i2s_new_channel(&cc, &s_tx, NULL);
    if (e != ESP_OK)
        return e;

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)AUDIO_I2S_PIN_BCLK,
                .ws = (gpio_num_t)AUDIO_I2S_PIN_LRC,
                .dout = (gpio_num_t)AUDIO_I2S_PIN_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
            },
    };
    if ((e = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK || (e = i2s_channel_enable(s_tx)) != ESP_OK)
    {
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return e;
    }
    return ESP_OK;
}

esp_err_t audio_out_write(const int16_t *frames, size_t n_frames)
{
    if (!s_tx)
        return ESP_ERR_INVALID_STATE;
    const uint8_t *p = (const uint8_t *)frames;
    size_t left = n_frames * 2 * sizeof(int16_t);
    while (left > 0)
    {
        size_t written = 0;
        esp_err_t e = i2s_channel_write(s_tx, p, left, &written, I2S_WRITE_TIMEOUT_MS);
        if (e != ESP_OK)
            return e;
        p += written;
        left -= written;
    }
    return ESP_OK;
}

esp_err_t audio_out_drain(void)
{
    /* Pushing a whole DMA ring of silence only completes once everything queued before it has been clocked out. */
    for (int i = 0; i < I2S_DMA_DESC; i++)
    {
        esp_err_t e = audio_out_write(s_zeros, I2S_DMA_FRAMES);
        if (e != ESP_OK)
            return e;
    }
    return ESP_OK;
}
