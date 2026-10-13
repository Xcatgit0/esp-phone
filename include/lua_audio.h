#ifndef LUA_AUDIO_H
#define LUA_AUDIO_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lua API `audio` (thin layer over audio_player.h). Call once from API_INIT():  audio_register(L);
 *
 *   audio.play(source [, opts])     -> true | nil, err      source: "/fs/test.mp3" | "http(s)://host/stream.mp3"
 *       opts = { loop=false, timeout=15000, prebuffer=32768, headers={Name="value"}, ca_cert="PEM..." }
 *   audio.stop()   audio.pause()   audio.resume()
 *   audio.volume([v])               -> current volume; v = 0..1 (linear amplitude)
 *   audio.mono([bool])              -> current; true = (L+R)/2 on both channels (default)
 *   audio.eq([bands])               -> bands = { {type="peak", freq=100, gain=-6, q=1}, ... } (max 5); no argument: read
 *   audio.eq_band(i, freq, gain [, q [, type]])   audio.eq_band(i, nil)    audio.eq_reset()
 *   audio.rate([hz])                -> output sample rate (8000..48000), settable only while stopped
 *   audio.status()                  -> { state, uri, codec, position, duration, sample_rate, channels,
 *                                        out_rate, volume, mono, buffered, error }
 *   audio.wait([timeout_ms])        -> "finished" | "stopped" | "error", msg | nil (timeout); sleeps, no busy loop
 *   audio.codecs()                  -> { "wav", "mp3", ... } supported in this build
 */
void audio_register(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* LUA_AUDIO_H */
