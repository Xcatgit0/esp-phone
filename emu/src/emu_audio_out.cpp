/*
 * emu_audio_out.cpp - audio_out_*() backend for the EMULATOR (declaration: include/audio_out.h).
 *
 * The device sends 16-bit STEREO to the MAX98357 over I2S; the emulator mixes it to MONO and plays it through
 * PulseAudio (libpulse-simple). Other sinks, chosen with  phone_emu --audio <sink>:
 *     pulse               PulseAudio, mono (default; falls back to "null" if no server answers)
 *     null                no sound, paced in real time like the hardware
 *     wav:FILE.wav        record what PulseAudio would get (mono), paced in real time
 *     wav-stereo:FILE.wav record what the device sends over I2S (stereo), paced in real time
 * Like a DMA ring, write() only blocks when more than ~70 ms are queued, so timing matches the device.
 */
#include "emu/emu.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "audio_out.h"
}
#ifdef EMU_HAVE_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

using Clock = std::chrono::steady_clock;

namespace {
std::mutex g_m;
std::string g_sinkSpec = "pulse";
enum class Sink { Pulse, Null, Wav, WavStereo } g_sink = Sink::Pulse;
std::string g_wavPath;
bool g_open = false;
int g_rate = 44100;
FILE *g_wav = nullptr;
uint64_t g_wavFrames = 0;
Clock::time_point g_t0;
uint64_t g_frames = 0;           // frames accepted since open (for pacing)
constexpr double kAheadSec = 0.07;
bool g_warnedNoPulse = false;
#ifdef EMU_HAVE_PULSE
pa_simple *g_pa = nullptr;
#endif

void wavHeader(FILE *f, int rate, int channels, uint64_t frames) {
    uint32_t dataBytes = (uint32_t)(frames * channels * 2);
    unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 0, 0,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    auto w32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) h[o + i] = (v >> (8 * i)) & 0xFF; };
    h[22] = (unsigned char)channels;
    w32(4, 36 + dataBytes); w32(24, (uint32_t)rate); w32(28, (uint32_t)(rate * channels * 2));
    h[32] = (unsigned char)(channels * 2);
    w32(40, dataBytes);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, 0, SEEK_END);
}

void paceUntil(double playedSec) {          // sleep so we are never more than kAheadSec ahead of "real time"
    auto target = g_t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(playedSec));
    auto now = Clock::now();
    if (now > target + std::chrono::milliseconds(20)) g_t0 += now - target;   // we were late (paused/stalled): the hardware would have
                                                                               // played silence, it does not "catch up" afterwards
    else std::this_thread::sleep_until(target);
}
}  // namespace

namespace emu {
void setAudioSink(const std::string &spec) {
    std::lock_guard<std::mutex> l(g_m);
    g_sinkSpec = spec;
    if (spec == "null") g_sink = Sink::Null;
    else if (spec.rfind("wav-stereo:", 0) == 0) { g_sink = Sink::WavStereo; g_wavPath = spec.substr(11); }
    else if (spec.rfind("wav:", 0) == 0) { g_sink = Sink::Wav; g_wavPath = spec.substr(4); }
    else g_sink = Sink::Pulse;
}
}  // namespace emu

extern "C" {

esp_err_t audio_out_close(void) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_open) return ESP_OK;
#ifdef EMU_HAVE_PULSE
    if (g_pa) { int e; pa_simple_flush(g_pa, &e); pa_simple_free(g_pa); g_pa = nullptr; }
#endif
    if (g_wav) { wavHeader(g_wav, g_rate, g_sink == Sink::WavStereo ? 2 : 1, g_wavFrames); fflush(g_wav); }   // file stays open: the next track appends
    g_open = false;
    return ESP_OK;
}

esp_err_t audio_out_open(int rate) {
    audio_out_close();
    std::lock_guard<std::mutex> l(g_m);
    g_frames = 0; g_t0 = Clock::now();
    if (!g_wav) { g_rate = rate; g_wavFrames = 0; }
    else if (rate != g_rate) emu::log("audio: output rate changed %d -> %d Hz; the WAV recording keeps %d Hz", g_rate, rate, g_rate);
    Sink sink = g_sink;
    if (sink == Sink::Pulse) {
#ifdef EMU_HAVE_PULSE
        pa_sample_spec ss{PA_SAMPLE_S16LE, (uint32_t)rate, 1};
        pa_buffer_attr ba;
        ba.maxlength = (uint32_t)-1;
        ba.tlength = (uint32_t)(rate * 2 * kAheadSec);   // target ~70 ms of queued audio
        ba.prebuf = (uint32_t)-1; ba.minreq = (uint32_t)-1; ba.fragsize = (uint32_t)-1;
        int err = 0;
        g_pa = pa_simple_new(nullptr, "phone_emu", PA_STREAM_PLAYBACK, nullptr, "audio.play", &ss, nullptr, &ba, &err);
        if (!g_pa) {
            if (!g_warnedNoPulse) { emu::log("audio: PulseAudio unavailable (%s) -> silent \"null\" sink; use --audio wav:FILE to record", pa_strerror(err)); g_warnedNoPulse = true; }
            g_sink = Sink::Null;
        }
#else
        if (!g_warnedNoPulse) { emu::log("audio: built without PulseAudio (sudo apt install libpulse-dev, re-run cmake) -> silent \"null\" sink"); g_warnedNoPulse = true; }
        g_sink = Sink::Null;
#endif
    } else if ((sink == Sink::Wav || sink == Sink::WavStereo) && !g_wav) {
        g_wav = fopen(g_wavPath.c_str(), "wb");
        if (!g_wav) { emu::log("audio: cannot create %s", g_wavPath.c_str()); g_sink = Sink::Null; }
        else wavHeader(g_wav, rate, sink == Sink::WavStereo ? 2 : 1, 0);
    }
    g_open = true;
    return ESP_OK;
}

esp_err_t audio_out_write(const int16_t *frames, size_t n) {
    // (no lock while sleeping: close() from another thread must not deadlock; the audio task is the only writer)
    Sink sink; int rate;
    { std::lock_guard<std::mutex> l(g_m); if (!g_open) return ESP_ERR_INVALID_STATE; sink = g_sink; rate = g_rate; }
    if (sink == Sink::Pulse) rate = rate;   // (rate only matters for pacing/recording)

    std::vector<int16_t> mono;
    if (sink != Sink::WavStereo) {
        mono.resize(n);
        for (size_t i = 0; i < n; i++) mono[i] = (int16_t)(((int)frames[2 * i] + (int)frames[2 * i + 1]) / 2);   // I2S stereo -> emulator mono
    }
    if (sink == Sink::Pulse) {
#ifdef EMU_HAVE_PULSE
        int err;
        if (g_pa && pa_simple_write(g_pa, mono.data(), n * sizeof(int16_t), &err) < 0) { emu::log("audio: pulse write failed: %s", pa_strerror(err)); return ESP_FAIL; }
#endif
    } else {
        if (g_wav) {
            if (sink == Sink::WavStereo) fwrite(frames, 4, n, g_wav); else fwrite(mono.data(), 2, n, g_wav);
            g_wavFrames += n;
            wavHeader(g_wav, rate, sink == Sink::WavStereo ? 2 : 1, g_wavFrames);   // keep the file valid even if we get killed
            fflush(g_wav);
        }
        g_frames += n;
        paceUntil((double)g_frames / rate - kAheadSec);
    }
    return ESP_OK;
}

esp_err_t audio_out_drain(void) {
    Sink sink; int rate;
    { std::lock_guard<std::mutex> l(g_m); if (!g_open) return ESP_OK; sink = g_sink; rate = g_rate; }
    if (sink == Sink::Pulse) {
#ifdef EMU_HAVE_PULSE
        int err; if (g_pa) pa_simple_drain(g_pa, &err);
#endif
    } else paceUntil((double)g_frames / rate);
    return ESP_OK;
}

}  // extern "C"
