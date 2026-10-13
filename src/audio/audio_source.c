/*
 * audio_source.c - file and network (HTTP/HTTPS streaming) sources. Same code on the device and in the emulator.
 *
 * Network: a reader task ("audio_net", core 0) pulls the stream with esp_http_client into a ring buffer;
 * audio_source_open() returns once AUDIO_NET_PREBUFFER bytes (or the whole file) are in it. If the network
 * stalls, reads time out and the player simply keeps feeding silence until data arrives.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "wifi_core.h"
#include "audio_source.h"

#define NET_RING_BYTES (128 * 1024)
#define NET_PREBUFFER_DEFAULT (32 * 1024)
#define NET_TIMEOUT_DEFAULT 15000
#define NET_TASK_STACK 12288
#define NET_TASK_PRIO 5
#define NET_TASK_CORE 0
#define NET_MAX_URL 2048
#define NET_MAX_REDIRECTS 5
#define NET_RX_BUF 4096
#define NET_CHUNK 2048
#define NET_USER_AGENT "ESP32-Phone/1.0"

typedef enum { SRC_FILE = 0, SRC_HTTP } src_kind_t;

struct audio_source
{
    src_kind_t kind;
    int64_t size;
    char err[96];
    audio_cancel_fn cancel;
    void *cancel_arg;

    /* file */
    FILE *f;

    /* http */
    char url[NET_MAX_URL + 1];
    int timeout_ms;
    int n_hdr;
    char *hdr_name[8];
    char *hdr_value[8];
    char *ca_cert;
    size_t ca_cert_len;

    uint8_t *ring;
    size_t rd, wr, count;
    bool eof, failed, abort_flag, connected, done, orphan;
    SemaphoreHandle_t lock;
    QueueHandle_t data_q;  /* "data / state change" wake for the reader of the ring */
    QueueHandle_t space_q; /* "space available" wake for the network task */
    QueueHandle_t done_q;  /* the network task has finished */
};

static void src_seterr(audio_source_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void src_seterr(audio_source_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof(s->err), fmt, ap);
    va_end(ap);
}

bool audio_source_is_url(const char *uri)
{
    return uri && (strncasecmp(uri, "http://", 7) == 0 || strncasecmp(uri, "https://", 8) == 0);
}

static void signal_q(QueueHandle_t q)
{
    uint8_t one = 1;
    xQueueOverwrite(q, &one);
}

static bool is_cancelled(audio_source_t *s) { return s->cancel && s->cancel(s->cancel_arg); }

/* ------------------------------------------------------------------------ network task */

typedef struct
{
    char location[512];
} hop_ctx_t;

static esp_err_t net_event(esp_http_client_event_t *evt)
{
    hop_ctx_t *ctx = (hop_ctx_t *)evt->user_data;
    if (ctx && evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "location") == 0)
        snprintf(ctx->location, sizeof(ctx->location), "%s", evt->header_value);
    return ESP_OK;
}

static bool resolve_location(const char *base, const char *loc, char *out, size_t n)
{
    int w;
    if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0)
        w = snprintf(out, n, "%s", loc);
    else
    {
        const char *scheme_end = strstr(base, "://");
        if (!scheme_end)
            return false;
        const char *path = strchr(scheme_end + 3, '/');
        size_t origin = path ? (size_t)(path - base) : strlen(base);
        if (loc[0] == '/')
            w = snprintf(out, n, "%.*s%s", (int)origin, base, loc);
        else
        {
            size_t end = strcspn(base, "?#"), dir = origin;
            for (size_t i = end; i > origin; i--)
                if (base[i - 1] == '/')
                {
                    dir = i - 1;
                    break;
                }
            w = snprintf(out, n, "%.*s/%s", (int)dir, base, loc);
        }
    }
    return w > 0 && (size_t)w < n;
}

/* returns false if aborted while waiting for space */
static bool ring_put(audio_source_t *s, const uint8_t *data, size_t n)
{
    while (n > 0)
    {
        xSemaphoreTake(s->lock, portMAX_DELAY);
        if (s->abort_flag)
        {
            xSemaphoreGive(s->lock);
            return false;
        }
        size_t space = NET_RING_BYTES - s->count;
        size_t k = n < space ? n : space;
        for (size_t i = 0; i < k; i++)
        {
            s->ring[s->wr] = data[i];
            s->wr = (s->wr + 1) % NET_RING_BYTES;
        }
        s->count += k;
        xSemaphoreGive(s->lock);
        if (k)
            signal_q(s->data_q);
        data += k;
        n -= k;
        if (n > 0)
        {
            uint8_t w;
            xQueueReceive(s->space_q, &w, pdMS_TO_TICKS(100)); /* ring full: wait for the player to drain it */
        }
    }
    return true;
}

static void net_finish(audio_source_t *s, bool failed)
{
    xSemaphoreTake(s->lock, portMAX_DELAY);
    if (failed)
        s->failed = true;
    else
        s->eof = true;
    bool orphan = s->orphan;
    s->done = true;
    xSemaphoreGive(s->lock);
    signal_q(s->data_q);
    signal_q(s->done_q);
    if (orphan) /* audio_source_close() gave up waiting: we own the struct now */
    {
        free(s->ring);
        for (int i = 0; i < s->n_hdr; i++)
        {
            free(s->hdr_name[i]);
            free(s->hdr_value[i]);
        }
        free(s->ca_cert);
        free(s);
    }
}

static void net_task(void *arg)
{
    audio_source_t *s = (audio_source_t *)arg;
    char url[NET_MAX_URL + 1];
    snprintf(url, sizeof(url), "%s", s->url);
    bool failed = false;
    int stalled_ms = 0;

    for (int hop = 0; hop <= NET_MAX_REDIRECTS && !s->abort_flag; hop++)
    {
        hop_ctx_t ctx = {.location = {0}};
        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = s->timeout_ms,
            .event_handler = net_event,
            .user_data = &ctx,
            .disable_auto_redirect = true,
            .buffer_size = NET_RX_BUF,
            .buffer_size_tx = 1536,
            .user_agent = NET_USER_AGENT,
            .keep_alive_enable = false,
        };
        if (s->ca_cert)
        {
            cfg.cert_pem = s->ca_cert;
            cfg.cert_len = s->ca_cert_len;
        }
        else
            cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c)
        {
            src_seterr(s, "out of memory (http client)");
            failed = true;
            break;
        }
        for (int i = 0; i < s->n_hdr; i++)
            esp_http_client_set_header(c, s->hdr_name[i], s->hdr_value[i]);

        bool redirect = false;
        esp_err_t e = esp_http_client_open(c, 0);
        if (e != ESP_OK)
        {
            src_seterr(s, e == ESP_ERR_HTTP_CONNECTING ? "timed out while connecting" : "cannot connect: %s",
                       esp_err_to_name(e));
            failed = true;
        }
        else
        {
            int64_t clen = esp_http_client_fetch_headers(c);
            int status = esp_http_client_get_status_code(c);
            if (clen == -ESP_ERR_HTTP_EAGAIN)
            {
                src_seterr(s, "timed out waiting for the response");
                failed = true;
            }
            else if (clen < 0 && status <= 0)
            {
                src_seterr(s, "no response (connection closed or TLS error)");
                failed = true;
            }
            else if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && ctx.location[0])
            {
                char next[NET_MAX_URL + 1];
                if (!resolve_location(url, ctx.location, next, sizeof(next)))
                {
                    src_seterr(s, "bad redirect location");
                    failed = true;
                }
                else if (strncasecmp(url, "https://", 8) == 0 && strncasecmp(next, "https://", 8) != 0)
                {
                    src_seterr(s, "refusing redirect from https to http");
                    failed = true;
                }
                else
                {
                    memcpy(url, next, strlen(next) + 1);
                    redirect = true;
                }
            }
            else if (status < 200 || status >= 300)
            {
                src_seterr(s, "HTTP status %d", status);
                failed = true;
            }
            else
            {
                xSemaphoreTake(s->lock, portMAX_DELAY);
                s->size = clen > 0 ? clen : -1;
                s->connected = true;
                xSemaphoreGive(s->lock);
                signal_q(s->data_q);

                uint8_t chunk[NET_CHUNK];
                while (!s->abort_flag)
                {
                    int n = esp_http_client_read(c, (char *)chunk, (int)sizeof(chunk));
                    if (n == -ESP_ERR_HTTP_EAGAIN)
                    {
                        stalled_ms += s->timeout_ms;
                        if (stalled_ms >= 3 * s->timeout_ms)
                        {
                            src_seterr(s, "stream stalled (no data for %d ms)", stalled_ms);
                            failed = true;
                            break;
                        }
                        continue;
                    }
                    stalled_ms = 0;
                    if (n < 0)
                    {
                        src_seterr(s, "read error");
                        failed = true;
                        break;
                    }
                    if (n == 0)
                        break; /* end of stream */
                    if (!ring_put(s, chunk, (size_t)n))
                        break;
                }
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (!redirect)
            break;
        if (hop == NET_MAX_REDIRECTS)
        {
            src_seterr(s, "too many redirects");
            failed = true;
        }
    }
    net_finish(s, failed);
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------- open/close */

static void src_free(audio_source_t *s)
{
    if (s->f)
        fclose(s->f);
    free(s->ring);
    for (int i = 0; i < s->n_hdr; i++)
    {
        free(s->hdr_name[i]);
        free(s->hdr_value[i]);
    }
    free(s->ca_cert);
    free(s);
}

static audio_source_t *open_http(const char *uri, const audio_source_opts_t *o, char *err, size_t errn)
{
    wifi_core_sta_info_t sta = wifi_core_sta_get_info();
    if (!sta.has_ip)
    {
        snprintf(err, errn, "no network: STA has no IP (wifi.sta.connect first)");
        return NULL;
    }
    if (strlen(uri) > NET_MAX_URL)
    {
        snprintf(err, errn, "url too long");
        return NULL;
    }
    audio_source_t *s = (audio_source_t *)calloc(1, sizeof(*s));
    if (!s)
    {
        snprintf(err, errn, "out of memory");
        return NULL;
    }
    s->kind = SRC_HTTP;
    s->size = -1;
    s->cancel = o ? o->cancel : NULL;
    s->cancel_arg = o ? o->cancel_arg : NULL;
    snprintf(s->url, sizeof(s->url), "%s", uri);
    s->timeout_ms = (o && o->timeout_ms > 0) ? o->timeout_ms : NET_TIMEOUT_DEFAULT;
    size_t prebuf = (o && o->prebuffer_bytes) ? o->prebuffer_bytes : NET_PREBUFFER_DEFAULT;
    if (prebuf > NET_RING_BYTES / 2)
        prebuf = NET_RING_BYTES / 2;
    for (int i = 0; o && i < o->n_hdr && i < 8; i++)
    {
        s->hdr_name[i] = strdup(o->hdr_name[i]);
        s->hdr_value[i] = strdup(o->hdr_value[i]);
        s->n_hdr++;
    }
    if (o && o->ca_cert)
    {
        s->ca_cert = (char *)malloc(o->ca_cert_len);
        if (s->ca_cert)
        {
            memcpy(s->ca_cert, o->ca_cert, o->ca_cert_len);
            s->ca_cert_len = o->ca_cert_len;
        }
    }
    s->ring = (uint8_t *)malloc(NET_RING_BYTES);
    s->lock = xSemaphoreCreateMutex();
    s->data_q = xQueueCreate(1, 1);
    s->space_q = xQueueCreate(1, 1);
    s->done_q = xQueueCreate(1, 1);
    if (!s->ring || !s->lock || !s->data_q || !s->space_q || !s->done_q)
    {
        snprintf(err, errn, "out of memory (stream buffer)");
        src_free(s);
        return NULL;
    }
    if (xTaskCreatePinnedToCore(net_task, "audio_net", NET_TASK_STACK, s, NET_TASK_PRIO, NULL, NET_TASK_CORE) != pdPASS)
    {
        snprintf(err, errn, "cannot start the network task");
        src_free(s);
        return NULL;
    }

    /* wait for the connection + prebuffer */
    const int64_t deadline = esp_timer_get_time() + (int64_t)s->timeout_ms * 1000 + 2000000;
    for (;;)
    {
        xSemaphoreTake(s->lock, portMAX_DELAY);
        bool failed = s->failed, eof = s->eof, connected = s->connected;
        size_t count = s->count;
        xSemaphoreGive(s->lock);

        if (failed)
        {
            snprintf(err, errn, "%s", s->err[0] ? s->err : "network error");
            audio_source_close(s);
            return NULL;
        }
        if (connected && (count >= prebuf || eof))
            return s;
        if (is_cancelled(s) || esp_timer_get_time() > deadline)
        {
            snprintf(err, errn, is_cancelled(s) ? "cancelled" : "timed out opening the stream");
            audio_source_close(s);
            return NULL;
        }
        uint8_t w;
        xQueueReceive(s->data_q, &w, pdMS_TO_TICKS(50));
    }
}

audio_source_t *audio_source_open(const char *uri, const audio_source_opts_t *o, char *err, size_t errn)
{
    if (!uri || !*uri)
    {
        snprintf(err, errn, "empty source");
        return NULL;
    }
    if (audio_source_is_url(uri))
        return open_http(uri, o, err, errn);

    const char *path = strncasecmp(uri, "file://", 7) == 0 ? uri + 7 : uri;
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        snprintf(err, errn, "cannot open %s", path);
        return NULL;
    }
    audio_source_t *s = (audio_source_t *)calloc(1, sizeof(*s));
    if (!s)
    {
        fclose(f);
        snprintf(err, errn, "out of memory");
        return NULL;
    }
    s->kind = SRC_FILE;
    s->f = f;
    s->cancel = o ? o->cancel : NULL;
    s->cancel_arg = o ? o->cancel_arg : NULL;
    s->size = -1;
    if (fseek(f, 0, SEEK_END) == 0)
    {
        long n = ftell(f);
        if (n >= 0)
            s->size = n;
        fseek(f, 0, SEEK_SET);
    }
    return s;
}

void audio_source_abort(audio_source_t *s)
{
    if (!s || s->kind != SRC_HTTP)
        return;
    s->abort_flag = true;
    signal_q(s->data_q);
    signal_q(s->space_q);
}

void audio_source_close(audio_source_t *s)
{
    if (!s)
        return;
    if (s->kind == SRC_FILE)
    {
        src_free(s);
        return;
    }
    audio_source_abort(s);
    uint8_t w;
    bool done = false;
    for (int i = 0; i < 60 && !done; i++) /* up to ~6 s: a TLS read may still be in flight */
    {
        xSemaphoreTake(s->lock, portMAX_DELAY);
        done = s->done;
        xSemaphoreGive(s->lock);
        if (!done)
            xQueueReceive(s->done_q, &w, pdMS_TO_TICKS(100));
    }
    if (done)
    {
        src_free(s);
        return;
    }
    xSemaphoreTake(s->lock, portMAX_DELAY); /* the task will free the struct itself when it finally ends */
    bool now_done = s->done;
    s->orphan = !now_done;
    xSemaphoreGive(s->lock);
    if (now_done)
        src_free(s);
}

/* ---------------------------------------------------------------------------------- read */

int audio_source_read(audio_source_t *s, uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    if (is_cancelled(s))
        return AUDIO_SRC_CANCELLED;

    if (s->kind == SRC_FILE)
    {
        size_t r = fread(buf, 1, n, s->f);
        if (r > 0)
            return (int)r;
        if (ferror(s->f))
        {
            src_seterr(s, "file read error");
            return AUDIO_SRC_ERROR;
        }
        return AUDIO_SRC_EOF;
    }

    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;)
    {
        xSemaphoreTake(s->lock, portMAX_DELAY);
        if (s->count > 0)
        {
            size_t k = n < s->count ? n : s->count;
            for (size_t i = 0; i < k; i++)
            {
                buf[i] = s->ring[s->rd];
                s->rd = (s->rd + 1) % NET_RING_BYTES;
            }
            s->count -= k;
            xSemaphoreGive(s->lock);
            signal_q(s->space_q);
            return (int)k;
        }
        bool failed = s->failed, eof = s->eof, aborted = s->abort_flag;
        xSemaphoreGive(s->lock);

        if (failed)
            return AUDIO_SRC_ERROR;
        if (eof)
            return AUDIO_SRC_EOF;
        if (aborted || is_cancelled(s))
            return AUDIO_SRC_CANCELLED;
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0)
            return AUDIO_SRC_TIMEOUT;
        uint8_t w;
        xQueueReceive(s->data_q, &w, (TickType_t)((remaining / 1000 + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS));
    }
}

const char *audio_source_error(audio_source_t *s) { return s->err[0] ? s->err : "source error"; }
int64_t audio_source_size(audio_source_t *s) { return s->size; }

size_t audio_source_buffered(audio_source_t *s)
{
    if (s->kind != SRC_HTTP)
        return 0;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    size_t c = s->count;
    xSemaphoreGive(s->lock);
    return c;
}
