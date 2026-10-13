/*
 * dec_esp_codec.c - decoder on top of espressif/esp_audio_codec's "simple decoder" (device only).
 * Enable with  build_flags = -DAUDIO_USE_ESP_CODEC  and add the component, e.g. src/idf_component.yml:
 *     dependencies:
 *       espressif/esp_audio_codec: "^2.4.0"
 * The simple decoder accepts input of ANY size and parses the container itself, so it maps 1:1 onto
 * audio_decoder_decode(): MP3, AAC (ADTS), FLAC, OGG (Vorbis/Opus), M4A, ... Header-checked against
 * esp-adf-libs/esp_audio_codec 2.6.x. Not run on hardware by the author of this file.
 */
#ifdef AUDIO_USE_ESP_CODEC

#include <stdlib.h>
#include <string.h>
#include "audio_decoder.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"

typedef struct
{
    esp_audio_simple_dec_handle_t h;
    audio_pcm_fmt_t fmt;
    bool have_fmt;
} esp_ctx_t;

static void register_once(void)
{
    static bool done = false;
    if (done)
        return;
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    done = true;
}

static esp_audio_simple_dec_type_t type_for(int codec)
{
    switch ((audio_codec_t)codec)
    {
    case AUDIO_CODEC_MP3: return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    case AUDIO_CODEC_AAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    case AUDIO_CODEC_FLAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    case AUDIO_CODEC_OGG: return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    case AUDIO_CODEC_M4A: return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    case AUDIO_CODEC_WAV: return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    default: return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

static void *esp_open(int codec)
{
    register_once();
    esp_audio_simple_dec_type_t t = type_for(codec);
    if (t == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE || esp_audio_simple_check_audio_type(t) != ESP_AUDIO_ERR_OK)
        return NULL;
    esp_ctx_t *c = (esp_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    esp_audio_simple_dec_cfg_t cfg = {.dec_type = t, .dec_cfg = NULL, .cfg_size = 0, .use_frame_dec = false};
    if (esp_audio_simple_dec_open(&cfg, &c->h) != ESP_AUDIO_ERR_OK)
    {
        free(c);
        return NULL;
    }
    return c;
}

static audio_dec_status_t esp_decode(void *ctx, const uint8_t *in, size_t in_len, bool eof, size_t *consumed, int16_t *pcm,
                                     size_t max_frames, size_t *frames, audio_pcm_fmt_t *fmt)
{
    esp_ctx_t *c = (esp_ctx_t *)ctx;
    if (in_len == 0)
        return AUDIO_DEC_NEED_DATA;

    esp_audio_simple_dec_raw_t raw = {.buffer = (uint8_t *)in, .len = (uint32_t)in_len, .eos = eof};
    esp_audio_simple_dec_out_t out = {.buffer = (uint8_t *)pcm, .len = (uint32_t)(max_frames * 2 * sizeof(int16_t))};
    esp_audio_err_t r = esp_audio_simple_dec_process(c->h, &raw, &out);

    if (r == ESP_AUDIO_ERR_DATA_LACK)
        return AUDIO_DEC_NEED_DATA;
    if (r != ESP_AUDIO_ERR_OK && r != ESP_AUDIO_ERR_CONTINUE)
        return AUDIO_DEC_ERROR; /* includes BUFF_NOT_ENOUGH: the format needs a bigger AUDIO_DEC_MAX_FRAMES */

    *consumed = raw.consumed;
    if (out.decoded_size > 0)
    {
        if (!c->have_fmt)
        {
            esp_audio_simple_dec_info_t info;
            if (esp_audio_simple_dec_get_info(c->h, &info) != ESP_AUDIO_ERR_OK || info.bits_per_sample != 16 ||
                info.channel < 1 || info.channel > 2)
                return AUDIO_DEC_ERROR; /* only 16-bit mono/stereo output is handled */
            c->fmt.sample_rate = (int)info.sample_rate;
            c->fmt.channels = info.channel;
            c->have_fmt = true;
        }
        *fmt = c->fmt;
        *frames = out.decoded_size / (sizeof(int16_t) * (size_t)c->fmt.channels);
    }
    return (raw.consumed > 0 || out.decoded_size > 0) ? AUDIO_DEC_OK : AUDIO_DEC_NEED_DATA;
}

static void esp_close(void *ctx)
{
    esp_ctx_t *c = (esp_ctx_t *)ctx;
    if (c->h)
        esp_audio_simple_dec_close(c->h);
    free(c);
}

const audio_decoder_ops_t audio_dec_esp_ops = {"esp_audio_codec", esp_open, esp_decode, NULL, esp_close};

#endif /* AUDIO_USE_ESP_CODEC */
