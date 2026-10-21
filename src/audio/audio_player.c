/*
 * audio_player.c - player core. Same code on the device and in the emulator; only audio_out_*() (I2S backend)
 * and the decoder implementation differ (see audio_out.h / audio_decoder.h).
 *
 *   play()  -> request slot -> "audio" task:
 *       open source -> sniff codec -> open decoder -> loop { refill input; decode; resample; EQ; volume; audio_out_write }
 *   Everything (decode + processing + output) runs in that one task; the I2S DMA buffer is the jitter buffer.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "wifi_core.h"
#include "audio_decoder.h"
#include "audio_out.h"
#include "audio_player.h"
#include "audio_source.h"

#ifndef AUDIO_TASK_STACK
#define AUDIO_TASK_STACK 32768 /* minimp3 keeps ~17 KB of scratch on the stack; 12288 is enough with esp_audio_codec */
#endif
#ifndef AUDIO_TASK_PRIO
#define AUDIO_TASK_PRIO 7
#endif
#ifndef AUDIO_TASK_CORE
#define AUDIO_TASK_CORE 0 /* Lua runs on core 1 */
#endif
#define AUDIO_IN_BUF 16384
#define AUDIO_VU_RELEASE_MS 220.0f /* time constant of the VU fall-off */
#define AUDIO_OUT_CHUNK 512 /* frames per audio_out_write() */
#define AUDIO_RAMP_FRAMES 441 /* volume changes ramp over ~10 ms (no zipper noise) */
#define AUDIO_VOLUME_DEFAULT 0.5f
#define AUDIO_NET_READ_TIMEOUT_MS 200

typedef struct
{
    char uri[256];
    bool loop;
    int timeout_ms;
    size_t prebuffer;
    int n_hdr;
    char hn[8][48];
    char hv[8][256];
    char *ca;
    size_t ca_len;
} req_t;

typedef struct
{
    float b0, b1, b2, a1, a2;
} biquad_t;

typedef struct
{
    biquad_t c;
    float z1[2], z2[2];
    bool active;
} eq_stage_t;

enum { TRACK_FINISHED = 0, TRACK_ABORTED, TRACK_ERROR };
typedef struct
{
    int kind;
    char err[96];
} track_result_t;

static struct
{
    bool inited;
    SemaphoreHandle_t lock;
    QueueHandle_t req_q, evq, pause_q;

    bool pending;
    volatile uint32_t gen;
    req_t req;

    audio_state_t state;
    char uri[256], codec[12], error[96];
    int64_t position_frames, duration_ms;
    int src_rate, src_ch;
    volatile bool paused;
    float volume;
    bool mono;
    int out_rate;
    audio_eq_band_t eq[AUDIO_EQ_BANDS];
    uint32_t eq_ver;
    audio_end_t last_end;
    char last_err[96];
    audio_source_t *cur_src;
    float vu[2];     /* peak level 0..1 of the last output chunk, decays with AUDIO_VU_RELEASE_MS */
    int64_t vu_ts;   /* esp_timer time (us) of that value */
} P;

static void signal_q(QueueHandle_t q)
{
    uint8_t one = 1;
    xQueueOverwrite(q, &one);
}

/* ------------------------------------------------------------------ EQ coefficients (RBJ cookbook) */

static void eq_calc(biquad_t *out, const audio_eq_band_t *b, int rate)
{
    float A = powf(10.0f, b->gain_db / 40.0f);
    float w0 = 6.28318531f * b->freq_hz / (float)rate;
    float cs = cosf(w0), sn = sinf(w0);
    float alpha = sn / (2.0f * b->q);
    float sq = 2.0f * sqrtf(A) * alpha;
    float b0, b1, b2, a0, a1, a2;
    switch (b->type)
    {
    case AUDIO_EQ_LOWSHELF:
        b0 = A * ((A + 1) - (A - 1) * cs + sq);
        b1 = 2 * A * ((A - 1) - (A + 1) * cs);
        b2 = A * ((A + 1) - (A - 1) * cs - sq);
        a0 = (A + 1) + (A - 1) * cs + sq;
        a1 = -2 * ((A - 1) + (A + 1) * cs);
        a2 = (A + 1) + (A - 1) * cs - sq;
        break;
    case AUDIO_EQ_HIGHSHELF:
        b0 = A * ((A + 1) + (A - 1) * cs + sq);
        b1 = -2 * A * ((A - 1) + (A + 1) * cs);
        b2 = A * ((A + 1) + (A - 1) * cs - sq);
        a0 = (A + 1) - (A - 1) * cs + sq;
        a1 = 2 * ((A - 1) - (A + 1) * cs);
        a2 = (A + 1) - (A - 1) * cs - sq;
        break;
    case AUDIO_EQ_LOWPASS:
        b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2;
        a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
        break;
    case AUDIO_EQ_HIGHPASS:
        b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2;
        a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
        break;
    case AUDIO_EQ_NOTCH:
        b0 = 1; b1 = -2 * cs; b2 = 1;
        a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
        break;
    case AUDIO_EQ_PEAK:
    default:
        b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A;
        break;
    }
    out->b0 = b0 / a0; out->b1 = b1 / a0; out->b2 = b2 / a0;
    out->a1 = a1 / a0; out->a2 = a2 / a0;
}

static bool eq_band_valid(const audio_eq_band_t *b, int rate)
{
    if (b->type < AUDIO_EQ_PEAK || b->type > AUDIO_EQ_NOTCH)
        return false;
    return b->freq_hz >= 20.0f && b->freq_hz <= 0.45f * (float)rate && b->gain_db >= -24.0f && b->gain_db <= 24.0f &&
           b->q >= 0.1f && b->q <= 20.0f;
}

/* ---------------------------------------------------------------------------------- track state */

typedef struct
{
    uint32_t gen;
    uint8_t *in;
    size_t in_len, in_pos;
    bool eof;
    int16_t *pcm;
    int errors;

    int src_rate;
    bool rs_init;
    int64_t rs_pos, rs_step;
    int16_t prev[2];

    float cur_gain, gain_target;
    bool mono;
    eq_stage_t eq[AUDIO_EQ_BANDS];
    uint32_t eq_ver_seen;
    int eq_rate;
    bool any_eq;

    int16_t out[AUDIO_OUT_CHUNK * 2];
    size_t out_n;
    bool out_open, aborted, out_failed;
    char err[96];
} track_t;

static bool track_cancelled(void *arg) { return P.gen != (uint32_t)(uintptr_t)arg; }

static bool flush_out(track_t *t)
{
    if (t->out_n == 0)
        return true;
    if (!t->out_open)
    {
        if (audio_out_open(P.out_rate) != ESP_OK)
        {
            snprintf(t->err, sizeof(t->err), "audio output failed to open");
            t->out_failed = true;
            return false;
        }
        t->out_open = true;
    }
    size_t n = t->out_n;
    t->out_n = 0;
    {
        /* VU: peak of this chunk, as sent to the amplifier (after EQ / volume) */
        int pl = 0, pr = 0;
        for (size_t i = 0; i < n; i++)
        {
            int l = t->out[i * 2], r = t->out[i * 2 + 1];
            if (l < 0) l = -l;
            if (r < 0) r = -r;
            if (l > pl) pl = l;
            if (r > pr) pr = r;
        }
        const int64_t now = esp_timer_get_time();
        xSemaphoreTake(P.lock, portMAX_DELAY);
        const float k = expf(-(float)(now - P.vu_ts) / (1000.0f * AUDIO_VU_RELEASE_MS));
        const float nl = (float)pl / 32768.0f, nr = (float)pr / 32768.0f;
        P.vu[0] = nl > P.vu[0] * k ? nl : P.vu[0] * k;
        P.vu[1] = nr > P.vu[1] * k ? nr : P.vu[1] * k;
        P.vu_ts = now;
        xSemaphoreGive(P.lock);
    }
    if (audio_out_write(t->out, n) != ESP_OK)
    {
        snprintf(t->err, sizeof(t->err), "audio output write failed");
        t->out_failed = true;
        return false;
    }
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.position_frames += (int64_t)n;
    xSemaphoreGive(P.lock);
    return !track_cancelled((void *)(uintptr_t)t->gen);
}

static inline bool emit_frame(track_t *t, float l, float r)
{
    if (t->mono)
        l = r = 0.5f * (l + r);
    if (t->any_eq)
    {
        for (int i = 0; i < AUDIO_EQ_BANDS; i++)
        {
            eq_stage_t *s = &t->eq[i];
            if (!s->active)
                continue;
            float y = s->c.b0 * l + s->z1[0];
            s->z1[0] = s->c.b1 * l - s->c.a1 * y + s->z2[0];
            s->z2[0] = s->c.b2 * l - s->c.a2 * y;
            l = y;
            y = s->c.b0 * r + s->z1[1];
            s->z1[1] = s->c.b1 * r - s->c.a1 * y + s->z2[1];
            s->z2[1] = s->c.b2 * r - s->c.a2 * y;
            r = y;
        }
    }
    float d = t->gain_target - t->cur_gain;
    const float step = 1.0f / (float)AUDIO_RAMP_FRAMES;
    if (d > step)
        t->cur_gain += step;
    else if (d < -step)
        t->cur_gain -= step;
    else
        t->cur_gain = t->gain_target;
    l *= t->cur_gain;
    r *= t->cur_gain;
    if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
    if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
    t->out[t->out_n * 2] = (int16_t)l;
    t->out[t->out_n * 2 + 1] = (int16_t)r;
    if (++t->out_n == AUDIO_OUT_CHUNK)
        return flush_out(t);
    return true;
}

static void refresh_params(track_t *t)
{
    audio_eq_band_t eq[AUDIO_EQ_BANDS];
    uint32_t ver;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    t->gain_target = P.volume;
    t->mono = P.mono;
    ver = P.eq_ver;
    memcpy(eq, P.eq, sizeof(eq));
    int rate = P.out_rate;
    xSemaphoreGive(P.lock);

    if (ver != t->eq_ver_seen || rate != t->eq_rate)
    {
        t->any_eq = false;
        for (int i = 0; i < AUDIO_EQ_BANDS; i++)
        {
            t->eq[i].active = eq[i].enabled && eq_band_valid(&eq[i], rate);
            if (t->eq[i].active)
            {
                eq_calc(&t->eq[i].c, &eq[i], rate);
                t->any_eq = true;
            }
        }
        t->eq_ver_seen = ver;
        t->eq_rate = rate;
    }
}

/* decoded block -> resample -> emit. Returns false on abort / output failure. */
static bool process_block(track_t *t, const int16_t *x, size_t n, int ch, int rate)
{
    refresh_params(t);
    const int out_rate = P.out_rate;
#define LS(i) ((float)x[(size_t)(i) * (size_t)ch])
#define RS(i) ((float)x[(size_t)(i) * (size_t)ch + (ch == 2 ? 1u : 0u)])
    if (rate == out_rate)
    {
        for (size_t i = 0; i < n; i++)
            if (!emit_frame(t, LS(i), RS(i)))
                return false;
        t->rs_init = false;
        return true;
    }
    if (!t->rs_init || rate != t->src_rate)
    {
        t->src_rate = rate;
        t->rs_step = (int64_t)(((uint64_t)rate << 32) / (uint64_t)out_rate);
        t->rs_pos = 0;
        t->prev[0] = x[0];
        t->prev[1] = (ch == 2) ? x[1] : x[0];
        t->rs_init = true;
    }
    /* linear interpolation; virtual sample k = -1 is the last frame of the previous block */
    int64_t pos = t->rs_pos;
    const int64_t N = (int64_t)n;
    while ((pos >> 32) <= N - 2)
    {
        int64_t k = pos >> 32;
        float f = (float)(uint32_t)(pos & 0xFFFFFFFFu) * (1.0f / 4294967296.0f);
        float l0 = k < 0 ? (float)t->prev[0] : LS(k), r0 = k < 0 ? (float)t->prev[1] : RS(k);
        float l1 = LS(k + 1), r1 = RS(k + 1);
        if (!emit_frame(t, l0 + (l1 - l0) * f, r0 + (r1 - r0) * f))
            return false;
        pos += t->rs_step;
    }
    t->rs_pos = pos - (N << 32);
    t->prev[0] = (int16_t)LS(N - 1);
    t->prev[1] = (int16_t)RS(N - 1);
#undef LS
#undef RS
    return true;
}

/* ------------------------------------------------------------------------------- the track */

static int fill_input(track_t *t, audio_source_t *src, uint32_t timeout_ms, char *err, size_t errn)
{
    if (t->in_pos > 0)
    {
        memmove(t->in, t->in + t->in_pos, t->in_len - t->in_pos);
        t->in_len -= t->in_pos;
        t->in_pos = 0;
    }
    if (t->in_len >= AUDIO_IN_BUF)
        return 0;
    int n = audio_source_read(src, t->in + t->in_len, AUDIO_IN_BUF - t->in_len, timeout_ms);
    if (n > 0)
    {
        t->in_len += (size_t)n;
        return 0;
    }
    if (n == AUDIO_SRC_EOF)
    {
        t->eof = true;
        return 0;
    }
    if (n == AUDIO_SRC_TIMEOUT)
        return 0;
    if (n == AUDIO_SRC_CANCELLED)
        return -2;
    snprintf(err, errn, "%s", audio_source_error(src));
    return -1;
}

static void src_opts_from_req(const req_t *r, uint32_t gen, audio_source_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->timeout_ms = r->timeout_ms;
    o->prebuffer_bytes = r->prebuffer;
    o->n_hdr = r->n_hdr;
    for (int i = 0; i < r->n_hdr; i++)
    {
        o->hdr_name[i] = r->hn[i];
        o->hdr_value[i] = r->hv[i];
    }
    o->ca_cert = r->ca;
    o->ca_cert_len = r->ca_len;
    o->cancel = track_cancelled;
    o->cancel_arg = (void *)(uintptr_t)gen;
}

static track_result_t run_track(const req_t *r, uint32_t gen)
{
    track_result_t res = {TRACK_ERROR, {0}};
    track_t *t = (track_t *)calloc(1, sizeof(*t));
    audio_source_t *src = NULL;
    audio_decoder_t *dec = NULL;
    if (!t)
    {
        snprintf(res.err, sizeof(res.err), "out of memory");
        return res;
    }
    t->gen = gen;
    t->in = (uint8_t *)malloc(AUDIO_IN_BUF);
    t->pcm = (int16_t *)malloc((size_t)AUDIO_DEC_MAX_FRAMES * 2 * sizeof(int16_t));
    t->cur_gain = -1.0f; /* set from the first block: no fade-in from 0 */
    if (!t->in || !t->pcm)
    {
        snprintf(res.err, sizeof(res.err), "out of memory");
        goto out;
    }

    audio_source_opts_t sopts;
    src_opts_from_req(r, gen, &sopts);
    char oerr[96] = {0};
    src = audio_source_open(r->uri, &sopts, oerr, sizeof(oerr));
    if (!src)
    {
        if (track_cancelled((void *)(uintptr_t)gen))
            res.kind = TRACK_ABORTED;
        else
            snprintf(res.err, sizeof(res.err), "%s", oerr);
        goto out;
    }
    xSemaphoreTake(P.lock, portMAX_DELAY);
    bool superseded = P.gen != gen;
    if (!superseded)
        P.cur_src = src;
    xSemaphoreGive(P.lock);
    if (superseded)
    {
        res.kind = TRACK_ABORTED;
        goto out;
    }

    bool need_more = true, announced = false;
    for (;;)
    {
        if (track_cancelled((void *)(uintptr_t)gen))
        {
            res.kind = TRACK_ABORTED;
            break;
        }
        while (P.paused && !track_cancelled((void *)(uintptr_t)gen))
        {
            uint8_t w;
            xQueueReceive(P.pause_q, &w, pdMS_TO_TICKS(50));
        }

        size_t avail = t->in_len - t->in_pos;
        if (!t->eof && (need_more || avail < AUDIO_IN_BUF / 2))
        {
            int fr = fill_input(t, src, audio_source_is_url(r->uri) ? AUDIO_NET_READ_TIMEOUT_MS : 1000, res.err, sizeof(res.err));
            if (fr == -2)
            {
                res.kind = TRACK_ABORTED;
                break;
            }
            if (fr == -1)
                goto out; /* res.kind == TRACK_ERROR */
            need_more = false;
            avail = t->in_len - t->in_pos;
            if (avail == 0 && !t->eof)
                continue; /* network underrun: the output plays silence until data arrives */
        }

        if (!dec) /* first bytes: find out what this is */
        {
            if (avail < 4096 && !t->eof)
            {
                need_more = true;
                continue;
            }
            audio_codec_t c = audio_codec_sniff(t->in + t->in_pos, avail, r->uri);
            dec = audio_decoder_open(c);
            if (!dec)
            {
                snprintf(res.err, sizeof(res.err), c == AUDIO_CODEC_UNKNOWN ? "unknown audio format" : "unsupported format: %s",
                         audio_codec_name(c));
                goto out;
            }
            xSemaphoreTake(P.lock, portMAX_DELAY);
            snprintf(P.codec, sizeof(P.codec), "%s", audio_codec_name(c));
            P.duration_ms = audio_decoder_duration_ms(dec);
            xSemaphoreGive(P.lock);
        }

        size_t consumed = 0, frames = 0;
        audio_pcm_fmt_t fmt = {0, 0};
        audio_dec_status_t st = audio_decoder_decode(dec, t->in + t->in_pos, avail, t->eof, &consumed, t->pcm,
                                                     AUDIO_DEC_MAX_FRAMES, &frames, &fmt);
        t->in_pos += consumed;

        if (frames > 0)
        {
            t->errors = 0;
            if (fmt.channels < 1 || fmt.channels > 2 || fmt.sample_rate < 8000 || fmt.sample_rate > 192000)
            {
                snprintf(res.err, sizeof(res.err), "unsupported stream format (%d Hz, %d ch)", fmt.sample_rate, fmt.channels);
                goto out;
            }
            if (!announced)
            {
                announced = true;
                xSemaphoreTake(P.lock, portMAX_DELAY);
                if (P.gen == gen)
                {
                    P.src_rate = fmt.sample_rate;
                    P.src_ch = fmt.channels;
                    P.duration_ms = audio_decoder_duration_ms(dec);
                    if (P.state == AUDIO_BUFFERING)
                        P.state = AUDIO_PLAYING;
                }
                xSemaphoreGive(P.lock);
            }
            if (t->cur_gain < 0)
            {
                refresh_params(t);
                t->cur_gain = t->gain_target;
            }
            if (!process_block(t, t->pcm, frames, fmt.channels, fmt.sample_rate))
            {
                if (t->out_failed)
                    snprintf(res.err, sizeof(res.err), "%s", t->err);
                else
                    res.kind = TRACK_ABORTED;
                if (t->out_failed)
                    goto out;
                break;
            }
        }

        if (st == AUDIO_DEC_ERROR)
        {
            if (++t->errors > 200)
            {
                snprintf(res.err, sizeof(res.err), "decode error");
                goto out;
            }
            if (consumed == 0)
                t->in_pos += 1; /* resync */
            continue;
        }
        if (st == AUDIO_DEC_NEED_DATA)
        {
            if (t->eof && t->in_pos >= t->in_len)
            {
                if (r->loop && !audio_source_is_url(r->uri)) /* restart the file */
                {
                    audio_decoder_close(dec);
                    dec = NULL;
                    audio_source_close(src);
                    src = NULL;
                    xSemaphoreTake(P.lock, portMAX_DELAY);
                    P.cur_src = NULL;
                    xSemaphoreGive(P.lock);
                    src = audio_source_open(r->uri, &sopts, oerr, sizeof(oerr));
                    if (!src)
                    {
                        snprintf(res.err, sizeof(res.err), "%s", oerr);
                        goto out;
                    }
                    xSemaphoreTake(P.lock, portMAX_DELAY);
                    P.cur_src = src;
                    xSemaphoreGive(P.lock);
                    t->in_len = t->in_pos = 0;
                    t->eof = false;
                    need_more = true;
                    continue;
                }
                res.kind = TRACK_FINISHED;
                break;
            }
            if (t->eof) /* decoder cannot use the rest */
            {
                res.kind = TRACK_FINISHED;
                break;
            }
            if (t->in_len - t->in_pos >= AUDIO_IN_BUF && consumed == 0)
            {
                snprintf(res.err, sizeof(res.err), "decoder stuck");
                goto out;
            }
            need_more = true;
        }
    }

    if (res.kind == TRACK_FINISHED)
    {
        flush_out(t);
        if (t->out_open)
            audio_out_drain();
    }

out:
    if (src)
    {
        xSemaphoreTake(P.lock, portMAX_DELAY);
        if (P.cur_src == src)
            P.cur_src = NULL;
        xSemaphoreGive(P.lock);
        audio_source_close(src);
    }
    if (dec)
        audio_decoder_close(dec);
    if (t)
    {
        if (t->out_open)
            audio_out_close();
        free(t->in);
        free(t->pcm);
        free(t);
    }
    return res;
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        uint8_t x;
        if (xQueueReceive(P.req_q, &x, portMAX_DELAY) != pdPASS)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        req_t r;
        uint32_t gen;
        xSemaphoreTake(P.lock, portMAX_DELAY);
        if (!P.pending)
        {
            xSemaphoreGive(P.lock);
            continue;
        }
        r = P.req;
        P.req.ca = NULL; /* ownership moved to the local copy */
        P.pending = false;
        gen = P.gen;
        xSemaphoreGive(P.lock);

        track_result_t res = run_track(&r, gen);
        free(r.ca);

        xSemaphoreTake(P.lock, portMAX_DELAY);
        if (gen == P.gen) /* not superseded / stopped meanwhile */
        {
            if (res.kind == TRACK_ERROR)
            {
                P.state = AUDIO_ERROR;
                snprintf(P.error, sizeof(P.error), "%s", res.err);
                snprintf(P.last_err, sizeof(P.last_err), "%s", res.err);
                P.last_end = AUDIO_END_ERROR;
            }
            else
            {
                P.state = AUDIO_STOPPED;
                P.last_end = res.kind == TRACK_FINISHED ? AUDIO_END_FINISHED : AUDIO_END_STOPPED;
            }
            P.paused = false;
        }
        xSemaphoreGive(P.lock);
        signal_q(P.evq);
    }
}

/* ----------------------------------------------------------------------------- public API */

esp_err_t audio_player_init(void)
{
    if (P.inited)
        return ESP_OK;
    P.lock = xSemaphoreCreateMutex();
    P.req_q = xQueueCreate(1, 1);
    P.evq = xQueueCreate(1, 1);
    P.pause_q = xQueueCreate(1, 1);
    if (!P.lock || !P.req_q || !P.evq || !P.pause_q)
        return ESP_ERR_NO_MEM;
    P.volume = AUDIO_VOLUME_DEFAULT;
    P.mono = true;
    P.out_rate = AUDIO_OUT_RATE_DEFAULT;
    P.duration_ms = -1;
    P.last_end = AUDIO_END_STOPPED;
    for (int i = 0; i < AUDIO_EQ_BANDS; i++)
        P.eq[i] = (audio_eq_band_t){false, AUDIO_EQ_PEAK, 1000.0f, 0.0f, 1.0f};
    if (xTaskCreatePinnedToCore(audio_task, "audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO, NULL, AUDIO_TASK_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    P.inited = true;
    return ESP_OK;
}

int audio_player_play(const char *uri, const audio_play_opts_t *o, char *err, size_t errn)
{
    if (audio_player_init() != ESP_OK)
    {
        snprintf(err, errn, "audio player: out of memory");
        return -1;
    }
    if (!uri || !*uri || strlen(uri) >= sizeof(P.req.uri))
    {
        snprintf(err, errn, "source must be a path or URL (max %d characters)", (int)sizeof(P.req.uri) - 1);
        return -1;
    }
    if (audio_source_is_url(uri))
    {
        if (!wifi_core_sta_get_info().has_ip)
        {
            snprintf(err, errn, "no network: STA has no IP (wifi.sta.connect first)");
            return -1;
        }
    }
    else
    {
        const char *path = strncmp(uri, "file://", 7) == 0 ? uri + 7 : uri;
        FILE *f = fopen(path, "rb");
        if (!f)
        {
            snprintf(err, errn, "cannot open %s", path);
            return -1;
        }
        fclose(f);
    }

    req_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.uri, sizeof(r.uri), "%s", uri);
    if (o)
    {
        r.loop = o->loop;
        r.timeout_ms = o->timeout_ms;
        r.prebuffer = o->prebuffer_bytes;
        for (int i = 0; i < o->n_hdr && i < 8; i++)
        {
            snprintf(r.hn[i], sizeof(r.hn[i]), "%s", o->hdr_name[i]);
            snprintf(r.hv[i], sizeof(r.hv[i]), "%s", o->hdr_value[i]);
            r.n_hdr++;
        }
        if (o->ca_cert)
        {
            r.ca_len = strlen(o->ca_cert) + 1;
            r.ca = (char *)malloc(r.ca_len);
            if (!r.ca)
            {
                snprintf(err, errn, "out of memory");
                return -1;
            }
            memcpy(r.ca, o->ca_cert, r.ca_len);
        }
    }

    xSemaphoreTake(P.lock, portMAX_DELAY);
    free(P.req.ca); /* a request that was never picked up */
    P.req = r;
    P.gen++;
    P.pending = true;
    P.state = AUDIO_BUFFERING;
    P.paused = false;
    P.error[0] = 0;
    P.codec[0] = 0;
    P.position_frames = 0;
    P.duration_ms = -1;
    P.src_rate = P.src_ch = 0;
    snprintf(P.uri, sizeof(P.uri), "%s", uri);
    if (P.cur_src)
        audio_source_abort(P.cur_src);
    xSemaphoreGive(P.lock);
    signal_q(P.pause_q);
    signal_q(P.req_q);
    return 0;
}

void audio_player_stop(void)
{
    if (!P.inited)
        return;
    bool was_active;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    was_active = P.pending || P.state == AUDIO_BUFFERING || P.state == AUDIO_PLAYING || P.state == AUDIO_PAUSED;
    P.gen++;
    P.pending = false;
    free(P.req.ca);
    P.req.ca = NULL;
    P.state = AUDIO_STOPPED;
    P.paused = false;
    if (was_active)
        P.last_end = AUDIO_END_STOPPED;
    if (P.cur_src)
        audio_source_abort(P.cur_src);
    xSemaphoreGive(P.lock);
    signal_q(P.pause_q);
    signal_q(P.evq);
}

void audio_player_pause(void)
{
    if (!P.inited)
        return;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    if (P.state == AUDIO_PLAYING || P.state == AUDIO_BUFFERING)
    {
        P.paused = true;
        P.state = AUDIO_PAUSED;
    }
    xSemaphoreGive(P.lock);
}

void audio_player_resume(void)
{
    if (!P.inited)
        return;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    if (P.state == AUDIO_PAUSED)
    {
        P.paused = false;
        P.state = (P.src_rate > 0) ? AUDIO_PLAYING : AUDIO_BUFFERING;
    }
    xSemaphoreGive(P.lock);
    signal_q(P.pause_q);
}

void audio_player_set_volume(float v)
{
    audio_player_init();
    if (!(v >= 0.0f))
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.volume = v;
    xSemaphoreGive(P.lock);
}

float audio_player_get_volume(void)
{
    audio_player_init();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    float v = P.volume;
    xSemaphoreGive(P.lock);
    return v;
}

void audio_player_set_mono(bool mono)
{
    audio_player_init();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.mono = mono;
    xSemaphoreGive(P.lock);
}

bool audio_player_get_mono(void)
{
    audio_player_init();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    bool m = P.mono;
    xSemaphoreGive(P.lock);
    return m;
}

int audio_player_set_eq(const audio_eq_band_t *bands, int n)
{
    audio_player_init();
    if (n < 0 || n > AUDIO_EQ_BANDS)
        return -1;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    for (int i = 0; i < n; i++)
        if (!eq_band_valid(&bands[i], P.out_rate))
        {
            xSemaphoreGive(P.lock);
            return -1;
        }
    for (int i = 0; i < AUDIO_EQ_BANDS; i++)
    {
        if (i < n)
        {
            P.eq[i] = bands[i];
            P.eq[i].enabled = true;
        }
        else
            P.eq[i].enabled = false;
    }
    P.eq_ver++;
    xSemaphoreGive(P.lock);
    return 0;
}

int audio_player_set_eq_band(int index, const audio_eq_band_t *band)
{
    audio_player_init();
    if (index < 0 || index >= AUDIO_EQ_BANDS)
        return -1;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    if (band->enabled && !eq_band_valid(band, P.out_rate))
    {
        xSemaphoreGive(P.lock);
        return -1;
    }
    P.eq[index] = *band;
    P.eq_ver++;
    xSemaphoreGive(P.lock);
    return 0;
}

void audio_player_eq_reset(void)
{
    audio_player_init();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    for (int i = 0; i < AUDIO_EQ_BANDS; i++)
        P.eq[i].enabled = false;
    P.eq_ver++;
    xSemaphoreGive(P.lock);
}

int audio_player_get_eq(audio_eq_band_t *bands)
{
    audio_player_init();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    memcpy(bands, P.eq, sizeof(P.eq));
    xSemaphoreGive(P.lock);
    return AUDIO_EQ_BANDS;
}

int audio_player_set_rate(int hz)
{
    audio_player_init();
    if (hz < 8000 || hz > 48000)
        return -1;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    bool busy = P.pending || P.state == AUDIO_BUFFERING || P.state == AUDIO_PLAYING || P.state == AUDIO_PAUSED;
    if (!busy)
        P.out_rate = hz;
    xSemaphoreGive(P.lock);
    return busy ? -1 : 0;
}

void audio_player_status(audio_status_t *o)
{
    memset(o, 0, sizeof(*o));
    audio_player_init();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    o->state = P.state;
    snprintf(o->uri, sizeof(o->uri), "%s", P.uri);
    snprintf(o->codec, sizeof(o->codec), "%s", P.codec);
    snprintf(o->error, sizeof(o->error), "%s", P.error);
    o->out_rate = P.out_rate;
    o->position_ms = P.position_frames * 1000 / P.out_rate;
    o->duration_ms = P.duration_ms;
    o->src_rate = P.src_rate;
    o->src_channels = P.src_ch;
    o->volume = P.volume;
    o->mono = P.mono;
    o->buffered_bytes = P.cur_src ? audio_source_buffered(P.cur_src) : 0;
    xSemaphoreGive(P.lock);
}

audio_end_t audio_player_wait(int64_t timeout_ms, char *err, size_t errn)
{
    audio_player_init();
    const int64_t deadline = timeout_ms < 0 ? 0 : esp_timer_get_time() + timeout_ms * 1000;
    for (;;)
    {
        xSemaphoreTake(P.lock, portMAX_DELAY);
        bool active = P.pending || P.state == AUDIO_BUFFERING || P.state == AUDIO_PLAYING || P.state == AUDIO_PAUSED;
        audio_end_t e = P.last_end;
        if (!active && err && errn)
            snprintf(err, errn, "%s", e == AUDIO_END_ERROR ? P.last_err : "");
        xSemaphoreGive(P.lock);
        if (!active)
            return e;

        TickType_t wait = portMAX_DELAY;
        if (timeout_ms >= 0)
        {
            int64_t rem = deadline - esp_timer_get_time();
            if (rem <= 0)
                return AUDIO_END_NONE;
            wait = (TickType_t)((rem / 1000 + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
        }
        uint8_t w;
        if (xQueueReceive(P.evq, &w, wait) != pdPASS && timeout_ms < 0)
            return AUDIO_END_NONE; /* interrupted (emulator reset/quit) */
    }
}

void audio_player_vm_reset(void)
{
    if (!P.inited)
        return;
    audio_player_stop();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.volume = AUDIO_VOLUME_DEFAULT;
    P.mono = true;
    P.out_rate = AUDIO_OUT_RATE_DEFAULT;
    for (int i = 0; i < AUDIO_EQ_BANDS; i++)
        P.eq[i].enabled = false;
    P.eq_ver++;
    P.last_end = AUDIO_END_STOPPED;
    P.error[0] = 0;
    xSemaphoreGive(P.lock);
}

/* Current output level (peak, linear 0..1) per channel. It falls off smoothly when nothing is being played. */
void audio_player_get_vu(float *left, float *right)
{
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    const float k = expf(-(float)(now - P.vu_ts) / (1000.0f * AUDIO_VU_RELEASE_MS));
    const float l = P.vu[0] * k, r = P.vu[1] * k;
    xSemaphoreGive(P.lock);
    if (left) *left = l;
    if (right) *right = r;
}
