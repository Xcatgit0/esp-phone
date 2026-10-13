/* audio_decoder.c - codec detection and dispatch to the decoder implementation compiled for this side. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "audio_decoder.h"

struct audio_decoder
{
    const audio_decoder_ops_t *ops;
    void *ctx;
};

static bool has_ext(const char *name, const char *ext)
{
    if (!name)
        return false;
    size_t n = strcspn(name, "?#"); /* ignore a URL query */
    size_t e = strlen(ext);
    return n >= e && strncasecmp(name + n - e, ext, e) == 0;
}

audio_codec_t audio_codec_sniff(const uint8_t *h, size_t len, const char *name)
{
    if (len >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4))
        return AUDIO_CODEC_WAV;
    if (len >= 4 && !memcmp(h, "fLaC", 4))
        return AUDIO_CODEC_FLAC;
    if (len >= 4 && !memcmp(h, "OggS", 4))
        return AUDIO_CODEC_OGG;
    if (len >= 8 && !memcmp(h + 4, "ftyp", 4))
        return AUDIO_CODEC_M4A;
    if (len >= 3 && !memcmp(h, "ID3", 3))
        return has_ext(name, ".aac") ? AUDIO_CODEC_AAC : AUDIO_CODEC_MP3;
    if (len >= 2 && h[0] == 0xFF && (h[1] & 0xF6) == 0xF0) /* ADTS: sync + layer bits 00 */
        return AUDIO_CODEC_AAC;
    if (len >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) /* MPEG audio frame sync */
        return AUDIO_CODEC_MP3;

    if (has_ext(name, ".wav"))
        return AUDIO_CODEC_WAV;
    if (has_ext(name, ".mp3"))
        return AUDIO_CODEC_MP3;
    if (has_ext(name, ".flac"))
        return AUDIO_CODEC_FLAC;
    if (has_ext(name, ".ogg") || has_ext(name, ".opus"))
        return AUDIO_CODEC_OGG;
    if (has_ext(name, ".m4a") || has_ext(name, ".mp4"))
        return AUDIO_CODEC_M4A;
    if (has_ext(name, ".aac"))
        return AUDIO_CODEC_AAC;
    return AUDIO_CODEC_UNKNOWN;
}

const char *audio_codec_name(audio_codec_t c)
{
    switch (c)
    {
    case AUDIO_CODEC_WAV: return "wav";
    case AUDIO_CODEC_MP3: return "mp3";
    case AUDIO_CODEC_AAC: return "aac";
    case AUDIO_CODEC_FLAC: return "flac";
    case AUDIO_CODEC_OGG: return "ogg";
    case AUDIO_CODEC_M4A: return "m4a";
    default: return "unknown";
    }
}

static const audio_decoder_ops_t *ops_for(audio_codec_t c)
{
    if (c == AUDIO_CODEC_WAV)
        return &audio_dec_wav_ops;
#ifdef AUDIO_USE_ESP_CODEC
    if (c != AUDIO_CODEC_UNKNOWN)
        return &audio_dec_esp_ops;
#else
    if (c == AUDIO_CODEC_MP3)
        return &audio_dec_minimp3_ops;
#endif
    return NULL;
}

bool audio_decoder_supported(audio_codec_t c) { return ops_for(c) != NULL; }

audio_decoder_t *audio_decoder_open(audio_codec_t c)
{
    const audio_decoder_ops_t *ops = ops_for(c);
    if (!ops)
        return NULL;
    audio_decoder_t *d = (audio_decoder_t *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->ops = ops;
    d->ctx = ops->open(c == AUDIO_CODEC_UNKNOWN ? 0 : (int)c);
    if (!d->ctx)
    {
        free(d);
        return NULL;
    }
    return d;
}

audio_dec_status_t audio_decoder_decode(audio_decoder_t *d, const uint8_t *in, size_t in_len, bool eof, size_t *consumed,
                                        int16_t *pcm, size_t max_frames, size_t *frames, audio_pcm_fmt_t *fmt)
{
    *consumed = 0;
    *frames = 0;
    return d->ops->decode(d->ctx, in, in_len, eof, consumed, pcm, max_frames, frames, fmt);
}

int64_t audio_decoder_duration_ms(audio_decoder_t *d) { return d->ops->duration_ms ? d->ops->duration_ms(d->ctx) : -1; }

void audio_decoder_close(audio_decoder_t *d)
{
    if (!d)
        return;
    d->ops->close(d->ctx);
    free(d);
}
