#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H
/*
 * audio_player.h - the player core (C API). The Lua API in lua_audio.c is a thin layer over this, so any
 * other system of the OS can use the player without Lua. Identical on the device and in the emulator.
 *
 *     source (file | http)  ->  decoder  ->  mono/stereo  ->  resampler  ->  EQ  ->  volume  ->  audio_out (I2S)
 *     audio_source.h            audio_decoder.h                                                  audio_out.h
 *
 * One task ("audio") does decode + processing + output. Volume/EQ/mono changes take effect within ~one
 * DMA buffer (tens of ms). Calls below are thread-safe and never block (except audio_player_wait).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_OUT_RATE_DEFAULT 44100 /* every track is resampled to this rate (change with audio_player_set_rate) */
#define AUDIO_EQ_BANDS 5

typedef enum
{
    AUDIO_STOPPED = 0,
    AUDIO_BUFFERING, /* opening the source / prebuffering the network */
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_ERROR, /* the last track failed (see status.error); behaves like stopped */
} audio_state_t;

typedef enum
{
    AUDIO_END_NONE = 0, /* audio_player_wait() timed out */
    AUDIO_END_FINISHED, /* the track played to its end */
    AUDIO_END_STOPPED,  /* audio_player_stop() or replaced by another track */
    AUDIO_END_ERROR,
} audio_end_t;

typedef struct
{
    bool loop;               /* files only: restart at the end */
    int timeout_ms;          /* network timeout, 0 = default (15000) */
    size_t prebuffer_bytes;  /* network prebuffer, 0 = default (32 KiB) */
    int n_hdr;               /* network request headers */
    const char *hdr_name[8];
    const char *hdr_value[8];
    const char *ca_cert;     /* PEM string for a private CA / self-signed server */
} audio_play_opts_t;

typedef enum
{
    AUDIO_EQ_PEAK = 0, /* boost/cut gain_db around freq_hz (q = width) */
    AUDIO_EQ_LOWSHELF,
    AUDIO_EQ_HIGHSHELF,
    AUDIO_EQ_LOWPASS,
    AUDIO_EQ_HIGHPASS,
    AUDIO_EQ_NOTCH, /* squeeze one frequency out */
} audio_eq_type_t;

typedef struct
{
    bool enabled;
    audio_eq_type_t type;
    float freq_hz;
    float gain_db;
    float q;
} audio_eq_band_t;

typedef struct
{
    audio_state_t state;
    char uri[256];
    char codec[12];
    char error[96];
    int64_t position_ms;  /* audio handed to the output so far */
    int64_t duration_ms;  /* -1 = unknown */
    int src_rate, src_channels;
    int out_rate;
    float volume;
    bool mono;
    size_t buffered_bytes; /* network ring buffer fill */
} audio_status_t;

esp_err_t audio_player_init(void); /* optional, everything calls it when needed */
/* 0 = accepted (errors while opening a network stream arrive later through status/wait), else err is filled */
int audio_player_play(const char *uri, const audio_play_opts_t *opts, char *err, size_t errn);
void audio_player_stop(void);
void audio_player_pause(void);
void audio_player_resume(void);
void audio_player_set_volume(float v); /* 0..1 linear amplitude */
float audio_player_get_volume(void);
void audio_player_set_mono(bool mono); /* true: (L+R)/2 on both channels (right for a single speaker amp) */
bool audio_player_get_mono(void);
int audio_player_set_eq(const audio_eq_band_t *bands, int n); /* replaces all bands; 0 ok, -1 invalid */
int audio_player_set_eq_band(int index, const audio_eq_band_t *band);
void audio_player_eq_reset(void);
int audio_player_get_eq(audio_eq_band_t *bands); /* returns AUDIO_EQ_BANDS entries */
int audio_player_set_rate(int hz);               /* 8000..48000, only while stopped; 0 ok */
void audio_player_status(audio_status_t *out);
/* Sleep until the current track ends. If nothing is playing, returns the last end reason at once.
 * timeout_ms < 0 = forever. err (optional) receives the error text for AUDIO_END_ERROR. */
audio_end_t audio_player_wait(int64_t timeout_ms, char *err, size_t errn);
void audio_player_vm_reset(void); /* new Lua VM (emulator): stop and restore defaults */

#endif /* AUDIO_PLAYER_H */
