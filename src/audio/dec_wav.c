/* dec_wav.c - streaming WAV (RIFF) decoder: PCM 8/16/24/32-bit and IEEE float, mono/stereo. Same code on both sides. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "audio_decoder.h"

typedef enum { W_RIFF, W_CHUNK, W_SKIP, W_DATA, W_DONE } wav_state_t;

typedef struct
{
    wav_state_t st;
    uint32_t skip;
    int tag, channels, rate, bits, block_align;
    bool have_fmt, unknown_len;
    uint64_t remaining;
    int64_t duration_ms;
} wav_t;

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

static void *wav_open(int codec)
{
    (void)codec;
    return calloc(1, sizeof(wav_t));
}

static int16_t clip16(float f)
{
    if (f > 1.0f)
        f = 1.0f;
    if (f < -1.0f)
        f = -1.0f;
    return (int16_t)(f * 32767.0f);
}

static audio_dec_status_t wav_decode(void *ctx, const uint8_t *in, size_t in_len, bool eof, size_t *consumed, int16_t *pcm,
                                     size_t max_frames, size_t *frames, audio_pcm_fmt_t *fmt)
{
    wav_t *w = (wav_t *)ctx;
    (void)eof;

    switch (w->st)
    {
    case W_RIFF:
        if (in_len < 12)
            return AUDIO_DEC_NEED_DATA;
        if (memcmp(in, "RIFF", 4) != 0 || memcmp(in + 8, "WAVE", 4) != 0)
            return AUDIO_DEC_ERROR;
        *consumed = 12;
        w->st = W_CHUNK;
        return AUDIO_DEC_OK;

    case W_SKIP:
    {
        size_t n = in_len < w->skip ? in_len : w->skip;
        *consumed = n;
        w->skip -= (uint32_t)n;
        if (w->skip == 0)
            w->st = W_CHUNK;
        return n ? AUDIO_DEC_OK : AUDIO_DEC_NEED_DATA;
    }

    case W_CHUNK:
    {
        if (in_len < 8)
            return AUDIO_DEC_NEED_DATA;
        uint32_t size = rd32(in + 4);
        uint32_t padded = size + (size & 1);
        if (!memcmp(in, "fmt ", 4))
        {
            if (size < 16 || size > 64)
                return AUDIO_DEC_ERROR;
            if (in_len < 8 + (size_t)padded)
                return AUDIO_DEC_NEED_DATA;
            const uint8_t *f = in + 8;
            w->tag = (int)rd16(f);
            w->channels = (int)rd16(f + 2);
            w->rate = (int)rd32(f + 4);
            w->block_align = (int)rd16(f + 12);
            w->bits = (int)rd16(f + 14);
            if (w->tag == 0xFFFE && size >= 40)
                w->tag = (int)rd16(f + 24); /* WAVE_FORMAT_EXTENSIBLE: real tag = first 2 bytes of the sub-format GUID */
            bool ok = (w->tag == 1 && (w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32)) ||
                      (w->tag == 3 && w->bits == 32);
            if (!ok || w->channels < 1 || w->rate < 1000 || w->rate > 192000 || w->block_align < 1 ||
                w->block_align < w->channels * (w->bits / 8))
                return AUDIO_DEC_ERROR;
            w->have_fmt = true;
            *consumed = 8 + (size_t)padded;
            return AUDIO_DEC_OK;
        }
        if (!memcmp(in, "data", 4))
        {
            if (!w->have_fmt)
                return AUDIO_DEC_ERROR;
            w->unknown_len = (size == 0 || size >= 0x7FFFFFF0u); /* streamed WAV: length not known */
            w->remaining = size;
            w->duration_ms = w->unknown_len ? -1 : (int64_t)((uint64_t)size * 1000 / ((uint64_t)w->rate * (uint64_t)w->block_align));
            w->st = W_DATA;
            *consumed = 8;
            return AUDIO_DEC_OK;
        }
        /* any other chunk (LIST, fact, ...): skip it */
        *consumed = 8;
        w->skip = padded;
        w->st = W_SKIP;
        return AUDIO_DEC_OK;
    }

    case W_DATA:
    {
        size_t avail = in_len;
        if (!w->unknown_len && avail > w->remaining)
            avail = (size_t)w->remaining;
        size_t bpf = (size_t)w->block_align;
        size_t n = avail / bpf;
        if (n > max_frames)
            n = max_frames;
        if (n == 0)
        {
            if (!w->unknown_len && w->remaining < bpf)
            {
                w->st = W_DONE; /* a partial last frame / trailing chunks: ignore */
                *consumed = in_len;
                return AUDIO_DEC_NEED_DATA;
            }
            return AUDIO_DEC_NEED_DATA;
        }
        int out_ch = w->channels > 1 ? 2 : 1;
        int bytes = w->bits / 8;
        for (size_t i = 0; i < n; i++)
        {
            const uint8_t *fr = in + i * bpf;
            for (int c = 0; c < out_ch; c++)
            {
                const uint8_t *s = fr + (size_t)c * (size_t)bytes;
                int16_t v;
                if (w->tag == 3)
                {
                    float f;
                    memcpy(&f, s, 4);
                    v = clip16(f);
                }
                else if (w->bits == 8)
                    v = (int16_t)(((int)s[0] - 128) << 8);
                else if (w->bits == 16)
                    v = (int16_t)rd16(s);
                else if (w->bits == 24)
                    v = (int16_t)rd16(s + 1);
                else
                    v = (int16_t)rd16(s + 2);
                pcm[i * (size_t)out_ch + (size_t)c] = v;
            }
        }
        *frames = n;
        *consumed = n * bpf;
        fmt->sample_rate = w->rate;
        fmt->channels = out_ch;
        if (!w->unknown_len)
        {
            w->remaining -= n * bpf;
            if (w->remaining < bpf)
            {
                w->st = W_DONE;
                *consumed = in_len < *consumed + w->remaining ? in_len : *consumed;
            }
        }
        return AUDIO_DEC_OK;
    }

    case W_DONE:
    default:
        *consumed = in_len; /* everything after the audio data is ignored */
        return AUDIO_DEC_NEED_DATA;
    }
}

static int64_t wav_duration(void *ctx) { return ((wav_t *)ctx)->duration_ms ? ((wav_t *)ctx)->duration_ms : -1; }
static void wav_close(void *ctx) { free(ctx); }

const audio_decoder_ops_t audio_dec_wav_ops = {"wav", wav_open, wav_decode, wav_duration, wav_close};
