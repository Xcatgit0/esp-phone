#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H
/*
 * audio_decoder.h - decoder interface of the audio player (same declaration on the device and in the emulator).
 *
 * The decoder is a PUSH decoder working on a byte stream, so the player does not care whether the bytes
 * come from a file or the network, or which library decodes them:
 *
 *     in = bytes read so far (the player keeps the unconsumed tail and appends new bytes)
 *     audio_decoder_decode(d, in, in_len, eof, &consumed, pcm, ..., &frames, &fmt)
 *       -> AUDIO_DEC_OK        progress (consumed > 0 and/or frames > 0), call again
 *       -> AUDIO_DEC_NEED_DATA nothing more can be done with `in`: keep the tail, append more bytes
 *       -> AUDIO_DEC_ERROR     bad data (the player skips a byte and retries; gives up after many errors)
 *
 * Which implementation is compiled is chosen per side:
 *   - emulator (default) and device (default):  WAV (dec_wav.c) + MP3 (dec_mp3_minimp3.c, minimp3)
 *   - device with -DAUDIO_USE_ESP_CODEC:        WAV (dec_wav.c) + everything else through esp_audio_codec's
 *                                               "simple decoder" (dec_esp_codec.c): MP3 AAC FLAC OGG M4A ...
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIO_CODEC_UNKNOWN = 0,
    AUDIO_CODEC_WAV,
    AUDIO_CODEC_MP3,
    AUDIO_CODEC_AAC,
    AUDIO_CODEC_FLAC,
    AUDIO_CODEC_OGG,
    AUDIO_CODEC_M4A,
} audio_codec_t;

typedef struct
{
    int sample_rate; /* Hz */
    int channels;    /* 1 or 2 (decoders downmix anything above) */
} audio_pcm_fmt_t;

typedef enum
{
    AUDIO_DEC_OK = 0,
    AUDIO_DEC_NEED_DATA,
    AUDIO_DEC_ERROR,
} audio_dec_status_t;

/* The PCM buffer given to audio_decoder_decode() must hold AUDIO_DEC_MAX_FRAMES frames * 2 channels * int16. */
#define AUDIO_DEC_MAX_FRAMES 4608

typedef struct audio_decoder audio_decoder_t;

/* Guess the codec from the first bytes (magic numbers), then from the file name / URL extension. */
audio_codec_t audio_codec_sniff(const uint8_t *head, size_t len, const char *name_hint);
const char *audio_codec_name(audio_codec_t c);
bool audio_decoder_supported(audio_codec_t c); /* in THIS build */

audio_decoder_t *audio_decoder_open(audio_codec_t c);
audio_dec_status_t audio_decoder_decode(audio_decoder_t *d, const uint8_t *in, size_t in_len, bool eof, size_t *consumed,
                                        int16_t *pcm, size_t max_frames, size_t *frames, audio_pcm_fmt_t *fmt);
int64_t audio_decoder_duration_ms(audio_decoder_t *d); /* -1 = unknown */
void audio_decoder_close(audio_decoder_t *d);

/* ---- for decoder implementers --------------------------------------------------------------- */
typedef struct
{
    const char *name;
    void *(*open)(int codec); /* audio_codec_t */
    audio_dec_status_t (*decode)(void *ctx, const uint8_t *in, size_t in_len, bool eof, size_t *consumed, int16_t *pcm,
                                 size_t max_frames, size_t *frames, audio_pcm_fmt_t *fmt);
    int64_t (*duration_ms)(void *ctx);
    void (*close)(void *ctx);
} audio_decoder_ops_t;

extern const audio_decoder_ops_t audio_dec_wav_ops;     /* dec_wav.c */
extern const audio_decoder_ops_t audio_dec_minimp3_ops; /* dec_mp3_minimp3.c (not built with AUDIO_USE_ESP_CODEC) */
extern const audio_decoder_ops_t audio_dec_esp_ops;     /* dec_esp_codec.c (only with AUDIO_USE_ESP_CODEC) */

#endif /* AUDIO_DECODER_H */
