/*
 * dec_mp3_minimp3.c - MP3 decoder on top of minimp3 (public domain / CC0, src/audio/minimp3.h).
 * Same code on the device and in the emulator unless the device is built with -DAUDIO_USE_ESP_CODEC.
 *
 * NOTE: mp3dec_decode_frame() keeps ~17 KB of scratch space on the STACK, so the audio task needs a big stack
 * (AUDIO_TASK_STACK in audio_player.c). SIMD is disabled so both sides produce bit-identical output.
 */
#ifndef AUDIO_USE_ESP_CODEC

#include <stdlib.h>
#include <string.h>
#include "audio_decoder.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define MP3_MIN_INPUT 4096 /* minimp3 wants several frames in the buffer to lock onto a stream reliably */

typedef struct
{
    mp3dec_t dec;
    uint32_t skip;
    bool id3_checked;
} mp3_t;

static void *mp3_open(int codec)
{
    (void)codec;
    mp3_t *m = (mp3_t *)calloc(1, sizeof(*m));
    if (m)
        mp3dec_init(&m->dec);
    return m;
}

static audio_dec_status_t mp3_decode(void *ctx, const uint8_t *in, size_t in_len, bool eof, size_t *consumed, int16_t *pcm,
                                     size_t max_frames, size_t *frames, audio_pcm_fmt_t *fmt)
{
    mp3_t *m = (mp3_t *)ctx;
    if (max_frames < 1152)
        return AUDIO_DEC_ERROR;

    if (m->skip) /* inside an ID3v2 tag */
    {
        size_t n = in_len < m->skip ? in_len : m->skip;
        *consumed = n;
        m->skip -= (uint32_t)n;
        return n ? AUDIO_DEC_OK : AUDIO_DEC_NEED_DATA;
    }
    if (!m->id3_checked)
    {
        if (in_len < 10 && !eof)
            return AUDIO_DEC_NEED_DATA;
        m->id3_checked = true;
        if (in_len >= 10 && !memcmp(in, "ID3", 3))
        {
            uint32_t size = ((uint32_t)(in[6] & 0x7F) << 21) | ((uint32_t)(in[7] & 0x7F) << 14) |
                            ((uint32_t)(in[8] & 0x7F) << 7) | (uint32_t)(in[9] & 0x7F);
            m->skip = 10 + size + ((in[5] & 0x10) ? 10 : 0);
            size_t n = in_len < m->skip ? in_len : m->skip;
            *consumed = n;
            m->skip -= (uint32_t)n;
            return AUDIO_DEC_OK;
        }
    }
    if (!eof && in_len < MP3_MIN_INPUT)
        return AUDIO_DEC_NEED_DATA;
    if (in_len == 0)
        return AUDIO_DEC_NEED_DATA;

    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&m->dec, in, (int)in_len, (mp3d_sample_t *)pcm, &info);
    *consumed = (size_t)info.frame_bytes;
    if (samples > 0)
    {
        *frames = (size_t)samples;
        fmt->sample_rate = info.hz;
        fmt->channels = info.channels;
        return AUDIO_DEC_OK;
    }
    if (info.frame_bytes > 0)
        return AUDIO_DEC_OK; /* skipped junk / a damaged frame */
    if (eof)
        *consumed = in_len; /* trailing garbage (ID3v1 tag, truncated frame) */
    return AUDIO_DEC_NEED_DATA;
}

static void mp3_close(void *ctx) { free(ctx); }

const audio_decoder_ops_t audio_dec_minimp3_ops = {"minimp3", mp3_open, mp3_decode, NULL, mp3_close};

#endif /* !AUDIO_USE_ESP_CODEC */
