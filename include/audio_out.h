#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H
/*
 * audio_out.h - I2S output backend of the audio player (same declaration on the device and in the emulator).
 *
 *   device    src/audio/out_i2s_esp.c          ESP-IDF I2S std driver -> MAX98357 (DOUT 4, BCLK 5, LRC 6)
 *   emulator  emu/src/emu_audio_out.cpp        PulseAudio (mono) / WAV file / null sink
 *
 * Format is always signed 16-bit STEREO interleaved (L,R,L,R...). The player already did resampling, EQ,
 * volume and (optionally) the mono downmix, so a backend only moves samples. The emulator backend mixes the
 * two channels to mono for PulseAudio; the device sends both channels to the amplifier.
 */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_I2S_PIN_DOUT 4 /* -> MAX98357 DIN  */
#define AUDIO_I2S_PIN_BCLK 5 /* -> MAX98357 BCLK */
#define AUDIO_I2S_PIN_LRC 6  /* -> MAX98357 LRC (word select) */

/* Start the clocks at `sample_rate`. Calling it while open re-opens with the new rate. */
esp_err_t audio_out_open(int sample_rate);
/* Queue frames (1 frame = L+R). Blocks while the hardware buffer is full (this is what paces the player). */
esp_err_t audio_out_write(const int16_t *frames, size_t n_frames);
/* Block until everything queued has been played. */
esp_err_t audio_out_drain(void);
/* Stop at once, dropping whatever is still queued. Safe to call when not open. */
esp_err_t audio_out_close(void);

#endif /* AUDIO_OUT_H */
