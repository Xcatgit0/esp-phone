/*
 * lua_http.c - wifi.http: HTTP/HTTPS client for the Lua API.
 * See include/lua_http.h for the Lua-level contract.
 *
 * Design
 *   - One worker task ("http", core 0, prio 4) executes requests one at a time, so only one TLS
 *     session is in RAM at once. Lua talks to it through up to HTTP_MAX_SLOTS request slots.
 *   - wifi.http.request() is start() + poll(id, forever). poll() sleeps on a queue; no busy loop.
 *   - Redirects are followed manually (fresh client per hop) so that https->http downgrades can
 *     be refused and the body of each hop never mixes with the next.
 *   - The body is read in chunks with esp_http_client_read(), so max_body, the overall deadline
 *     and cancel() are honoured while the download is in progress.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "lua.h"
#include "lauxlib.h"
#include "wifi_core.h"
#include "lua_http.h"

#define HTTP_MAX_SLOTS 4
#define HTTP_MAX_REQ_HEADERS 16
#define HTTP_MAX_RES_HEADERS 32
#define HTTP_RES_HDR_BYTES 4096
#define HTTP_MAX_URL 2048
#define HTTP_MAX_REQ_BODY (64 * 1024)
#define HTTP_DEF_TIMEOUT_MS 15000
#define HTTP_MIN_TIMEOUT_MS 100
#define HTTP_MAX_TIMEOUT_MS 120000
#define HTTP_SOCKET_TIMEOUT_MS 30000 /* per socket operation cap; the overall deadline is `timeout` */
#define HTTP_DEF_MAX_BODY (64 * 1024)
#define HTTP_HARD_MAX_BODY (1024 * 1024)
#define HTTP_DEF_REDIRECTS 5
#define HTTP_MAX_REDIRECTS 10
#define HTTP_RX_BUF 4096 /* GitHub & co. send multi-KB response headers; the 512 B default is too small */
#define HTTP_TX_BUF 1536
#define HTTP_TASK_STACK 12288 /* TLS handshake needs several KB of stack */
#define HTTP_TASK_PRIO 4
#define HTTP_TASK_CORE 0
#define HTTP_USER_AGENT "ESP32-Phone/1.0" /* api.github.com rejects requests without a User-Agent */

typedef enum { S_FREE = 0, S_QUEUED, S_RUNNING, S_DONE } slot_state_t;

typedef struct {
    char *name;
    char *value;
} hdr_t;

/* result, malloc'd by the worker, handed to Lua by poll() */
typedef struct {
    bool failed;
    char err[112];
    int status;
    int redirects;
    char *final_url;
    uint8_t *body;
    size_t body_len, body_cap;
    char *hdr; /* "name\0value\0name\0value\0..." */
    size_t hdr_len;
    int n_hdr;
} http_res_t;

typedef struct {
    slot_state_t state;
    uint32_t id;
    volatile bool cancel; /* set by cancel() or by a VM reset: abort and free the slot */

    /* request parameters (heap, owned by the slot until the worker is done with them) */
    char *url;
    esp_http_client_method_t method;
    hdr_t hdr[HTTP_MAX_REQ_HEADERS];
    int n_hdr;
    char *body;
    size_t body_len;
    char *ca_cert;
    size_t ca_cert_len;
    int timeout_ms;
    size_t max_body;
    int max_redirects;

    http_res_t *res;
} http_req_t;

static http_req_t s_req[HTTP_MAX_SLOTS];
static SemaphoreHandle_t s_lock = NULL;
static QueueHandle_t s_jobq = NULL;  /* slot indexes waiting for the worker */
static QueueHandle_t s_doneq = NULL; /* "something finished" wake-up for poll() */
static TaskHandle_t s_task = NULL;
static uint32_t s_next_id = 1;

/* ------------------------------------------------------------------------- helpers */

static void req_free_params(http_req_t *r)
{
    free(r->url);
    r->url = NULL;
    for (int i = 0; i < r->n_hdr; i++)
    {
        free(r->hdr[i].name);
        free(r->hdr[i].value);
    }
    r->n_hdr = 0;
    free(r->body);
    r->body = NULL;
    free(r->ca_cert);
    r->ca_cert = NULL;
}

static void res_free(http_res_t *res)
{
    if (!res)
        return;
    free(res->final_url);
    free(res->body);
    free(res->hdr);
    free(res);
}

static void slot_clear(http_req_t *r)
{
    req_free_params(r);
    res_free(r->res);
    memset(r, 0, sizeof(*r));
}

static char *dup_n(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static bool has_prefix_ci(const char *s, const char *prefix)
{
    return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_https(const char *url) { return has_prefix_ci(url, "https://"); }

static void res_fail(http_res_t *res, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void res_fail(http_res_t *res, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(res->err, sizeof(res->err), fmt, ap);
    va_end(ap);
    res->failed = true;
}

/* ----------------------------------------------------------------- response storage */

static void res_store_header(http_res_t *res, const char *k, const char *v)
{
    size_t kl = strlen(k), vl = strlen(v), need = kl + 1 + vl + 1;
    if (res->n_hdr >= HTTP_MAX_RES_HEADERS || res->hdr_len + need > HTTP_RES_HDR_BYTES)
        return; /* drop: headers are informational */
    char *nb = (char *)realloc(res->hdr, res->hdr_len + need);
    if (!nb)
        return;
    res->hdr = nb;
    char *p = nb + res->hdr_len;
    for (size_t i = 0; i < kl; i++)
        p[i] = (char)tolower((unsigned char)k[i]);
    p[kl] = 0;
    memcpy(p + kl + 1, v, vl + 1);
    res->hdr_len += need;
    res->n_hdr++;
}

static bool res_reserve(http_res_t *res, size_t want, size_t max_body)
{
    if (want <= res->body_cap)
        return true;
    size_t cap = res->body_cap ? res->body_cap * 2 : 4096;
    if (cap < want)
        cap = want;
    if (cap > max_body)
        cap = max_body;
    if (cap < want)
        return false;
    uint8_t *nb = (uint8_t *)realloc(res->body, cap);
    if (!nb)
        return false;
    res->body = nb;
    res->body_cap = cap;
    return true;
}

static void res_reset_hop(http_res_t *res)
{
    free(res->body);
    res->body = NULL;
    res->body_len = res->body_cap = 0;
    free(res->hdr);
    res->hdr = NULL;
    res->hdr_len = 0;
    res->n_hdr = 0;
    res->status = 0;
}

/* per-hop context handed to the esp_http_client event handler (lives on the worker's stack) */
typedef struct {
    http_res_t *res;
    char location[512];
} hop_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    hop_ctx_t *ctx = (hop_ctx_t *)evt->user_data;
    if (ctx && evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value)
    {
        res_store_header(ctx->res, evt->header_key, evt->header_value);
        if (strcasecmp(evt->header_key, "location") == 0)
            snprintf(ctx->location, sizeof(ctx->location), "%s", evt->header_value);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------ redirects */

static bool is_redirect_status(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

static bool resolve_location(const char *base, const char *loc, char *out, size_t n)
{
    int w;
    if (has_prefix_ci(loc, "http://") || has_prefix_ci(loc, "https://"))
    {
        w = snprintf(out, n, "%s", loc);
        return w > 0 && (size_t)w < n;
    }
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end)
        return false;
    const char *host = scheme_end + 3;
    const char *path = strchr(host, '/');
    size_t origin_len = path ? (size_t)(path - base) : strlen(base);

    if (loc[0] == '/' && loc[1] == '/')
        w = snprintf(out, n, "%.*s:%s", (int)(scheme_end - base), base, loc);
    else if (loc[0] == '/')
        w = snprintf(out, n, "%.*s%s", (int)origin_len, base, loc);
    else
    {
        /* relative to the directory of the base path (ignore ?query / #fragment of the base) */
        size_t base_len = strcspn(base, "?#");
        size_t dir_len = origin_len;
        for (size_t i = base_len; i > origin_len; i--)
            if (base[i - 1] == '/')
            {
                dir_len = i - 1;
                break;
            }
        w = snprintf(out, n, "%.*s/%s", (int)dir_len, base, loc);
    }
    return w > 0 && (size_t)w < n;
}

/* ------------------------------------------------------------------------- the worker */

/* One request/response exchange. Returns true if a response was received (res->status set). */
static bool do_hop(http_req_t *r, http_res_t *res, const char *url, esp_http_client_method_t method,
                   const char *body, size_t body_len, int64_t deadline_us, hop_ctx_t *ctx)
{
    int sock_timeout = r->timeout_ms < HTTP_SOCKET_TIMEOUT_MS ? r->timeout_ms : HTTP_SOCKET_TIMEOUT_MS;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = sock_timeout,
        .event_handler = http_event,
        .user_data = ctx,
        .disable_auto_redirect = true,
        .buffer_size = HTTP_RX_BUF,
        .buffer_size_tx = HTTP_TX_BUF,
        .user_agent = HTTP_USER_AGENT,
        .keep_alive_enable = false,
    };
    if (r->ca_cert)
    {
        cfg.cert_pem = r->ca_cert;
        cfg.cert_len = r->ca_cert_len;
    }
    else
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

    res_reset_hop(res);
    ctx->location[0] = 0;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c)
    {
        res_fail(res, "out of memory (http client)");
        return false;
    }
    for (int i = 0; i < r->n_hdr; i++)
        esp_http_client_set_header(c, r->hdr[i].name, r->hdr[i].value);

    bool ok = false;
    esp_err_t err = esp_http_client_open(c, (int)body_len);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_HTTP_CONNECTING)
            res_fail(res, "timed out while connecting");
        else
            res_fail(res, "cannot connect: %s", esp_err_to_name(err));
        goto done;
    }

    for (size_t off = 0; off < body_len;)
    {
        size_t n = body_len - off > 1024 ? 1024 : body_len - off;
        int w = esp_http_client_write(c, body + off, (int)n);
        if (w <= 0)
        {
            res_fail(res, "send failed");
            goto done;
        }
        off += (size_t)w;
    }

    int64_t clen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (clen == -ESP_ERR_HTTP_EAGAIN)
    {
        res_fail(res, "timed out waiting for the response");
        goto done;
    }
    if (clen < 0 && status <= 0)
    {
        res_fail(res, "no response (connection closed or TLS error)");
        goto done;
    }
    res->status = status;

    bool chunked = esp_http_client_is_chunked_response(c);
    if (clen > 0 && method != HTTP_METHOD_HEAD && (uint64_t)clen > r->max_body)
    {
        res_fail(res, "response too large: %lld bytes (max_body %u)", (long long)clen, (unsigned)r->max_body);
        goto done;
    }
    if (clen > 0 && !res_reserve(res, (size_t)clen, r->max_body))
    {
        res_fail(res, "out of memory for a %lld byte body", (long long)clen);
        goto done;
    }

    bool want_body = (method != HTTP_METHOD_HEAD) && status != 204 && status != 304;
    uint8_t chunk[1024];
    while (want_body)
    {
        if (r->cancel)
        {
            res_fail(res, "cancelled");
            goto done;
        }
        if (esp_timer_get_time() > deadline_us)
        {
            res_fail(res, "timed out");
            goto done;
        }
        int n = esp_http_client_read(c, (char *)chunk, (int)sizeof(chunk));
        if (n == -ESP_ERR_HTTP_EAGAIN)
            continue; /* nothing yet; deadline is checked at the top of the loop */
        if (n < 0)
        {
            res_fail(res, "read error");
            goto done;
        }
        if (n == 0)
        {
            if ((clen > 0 || chunked) && !esp_http_client_is_complete_data_received(c))
            {
                res_fail(res, "connection closed before the full body arrived");
                goto done;
            }
            break;
        }
        if (res->body_len + (size_t)n > r->max_body)
        {
            res_fail(res, "response exceeds max_body (%u bytes)", (unsigned)r->max_body);
            goto done;
        }
        if (!res_reserve(res, res->body_len + (size_t)n, r->max_body))
        {
            res_fail(res, "out of memory while reading the body");
            goto done;
        }
        memcpy(res->body + res->body_len, chunk, (size_t)n);
        res->body_len += (size_t)n;
    }
    ok = true;

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static void run_request(http_req_t *r)
{
    http_res_t *res = (http_res_t *)calloc(1, sizeof(*res));
    r->res = res;
    if (!res)
        return; /* poll() reports "out of memory" for a slot without a result */

    const int64_t deadline_us = esp_timer_get_time() + (int64_t)r->timeout_ms * 1000;
    esp_http_client_method_t method = r->method;
    const char *body = r->body;
    size_t body_len = r->body_len;

    char url[HTTP_MAX_URL + 1];
    snprintf(url, sizeof(url), "%s", r->url);
    hop_ctx_t ctx = {.res = res};

    for (;;)
    {
        if (r->cancel)
        {
            res_fail(res, "cancelled");
            return;
        }
        if (!do_hop(r, res, url, method, body, body_len, deadline_us, &ctx))
            return;
        if (!is_redirect_status(res->status) || r->max_redirects == 0 || ctx.location[0] == 0)
            break;
        if (res->redirects >= r->max_redirects)
        {
            res_fail(res, "too many redirects (%d)", r->max_redirects);
            return;
        }
        char next[HTTP_MAX_URL + 1];
        if (!resolve_location(url, ctx.location, next, sizeof(next)))
        {
            res_fail(res, "bad redirect location");
            return;
        }
        if (is_https(url) && !is_https(next))
        {
            res_fail(res, "refusing redirect from https to http");
            return;
        }
        if (res->status == 303 || ((res->status == 301 || res->status == 302) && method == HTTP_METHOD_POST))
        {
            if (method != HTTP_METHOD_HEAD)
                method = HTTP_METHOD_GET;
            body = NULL;
            body_len = 0;
        }
        memcpy(url, next, strlen(next) + 1);
        res->redirects++;
    }
    res->final_url = dup_n(url, strlen(url));
}

static void http_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        uint8_t idx;
        if (xQueueReceive(s_jobq, &idx, portMAX_DELAY) != pdPASS)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        http_req_t *r = &s_req[idx];

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool skip = r->cancel;
        if (!skip)
            r->state = S_RUNNING;
        xSemaphoreGive(s_lock);

        if (!skip)
            run_request(r); /* the slot belongs to the worker until it is marked DONE */

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (r->cancel)
            slot_clear(r); /* abandoned while it ran */
        else
        {
            req_free_params(r);
            r->state = S_DONE;
        }
        xSemaphoreGive(s_lock);

        uint8_t sig = idx;
        xQueueOverwrite(s_doneq, &sig);
    }
}

static bool http_init_once(void)
{
    if (s_task)
        return true;
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    if (!s_jobq)
        s_jobq = xQueueCreate(HTTP_MAX_SLOTS, sizeof(uint8_t));
    if (!s_doneq)
        s_doneq = xQueueCreate(1, sizeof(uint8_t));
    if (!s_lock || !s_jobq || !s_doneq)
        return false;
    return xTaskCreatePinnedToCore(http_task, "http", HTTP_TASK_STACK, NULL, HTTP_TASK_PRIO, &s_task,
                                   HTTP_TASK_CORE) == pdPASS;
}

/* New Lua VM (only happens in the emulator, a real reset reboots everything): drop old requests. */
static void http_vm_reset(void)
{
    if (!s_lock)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < HTTP_MAX_SLOTS; i++)
    {
        http_req_t *r = &s_req[i];
        if (r->state == S_QUEUED || r->state == S_RUNNING)
            r->cancel = true; /* worker frees the slot */
        else if (r->state == S_DONE)
            slot_clear(r);
    }
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- option parsing */

static bool valid_header_name(const char *s, size_t n)
{
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || strchr("!#$%&'*+-.^_`|~", c)))
            return false;
    }
    return true;
}

static bool header_is_managed(const char *name)
{
    static const char *const managed[] = {"host", "content-length", "transfer-encoding", "connection", NULL};
    for (int i = 0; managed[i]; i++)
        if (strcasecmp(name, managed[i]) == 0)
            return true;
    return false;
}

/* Never raises a Lua error (so nothing leaks): returns false and fills err. */
static bool parse_opts(lua_State *L, int opts, const char *url_in, const char *method_in, const char *body_in,
                       size_t body_len_in, bool body_given, http_req_t *r, char *err, size_t errn)
{
    memset(r, 0, sizeof(*r));
    r->timeout_ms = HTTP_DEF_TIMEOUT_MS;
    r->max_body = HTTP_DEF_MAX_BODY;
    r->max_redirects = HTTP_DEF_REDIRECTS;
    const char *url = url_in;
    size_t url_len = url_in ? strlen(url_in) : 0;
    const char *method = method_in ? method_in : "GET";
    const char *body = body_in;
    size_t body_len = body_len_in;
    bool have_body = body_given;
    bool ok = false;
    int top = lua_gettop(L);

    if (opts)
    {
        if (!url)
        {
            lua_getfield(L, opts, "url");
            if (lua_type(L, -1) == LUA_TSTRING)
                url = lua_tolstring(L, -1, &url_len);
            /* stays on the stack (keeps the pointer valid); popped via settop at the end */
        }
        lua_getfield(L, opts, "method");
        if (lua_type(L, -1) == LUA_TSTRING)
            method = lua_tostring(L, -1);
        else if (!lua_isnil(L, -1))
        {
            snprintf(err, errn, "method must be a string");
            goto out;
        }
        lua_getfield(L, opts, "body");
        if (lua_type(L, -1) == LUA_TSTRING)
        {
            body = lua_tolstring(L, -1, &body_len);
            have_body = true;
        }
        else if (!lua_isnil(L, -1))
        {
            snprintf(err, errn, "body must be a string");
            goto out;
        }
        lua_getfield(L, opts, "timeout");
        if (lua_type(L, -1) == LUA_TNUMBER)
        {
            lua_Number t = lua_tonumber(L, -1);
            r->timeout_ms = t < HTTP_MIN_TIMEOUT_MS ? HTTP_MIN_TIMEOUT_MS : t > HTTP_MAX_TIMEOUT_MS ? HTTP_MAX_TIMEOUT_MS : (int)t;
        }
        else if (!lua_isnil(L, -1))
        {
            snprintf(err, errn, "timeout must be a number (ms)");
            goto out;
        }
        lua_getfield(L, opts, "max_body");
        if (lua_type(L, -1) == LUA_TNUMBER)
        {
            lua_Number m = lua_tonumber(L, -1);
            r->max_body = m < 1 ? 1 : m > HTTP_HARD_MAX_BODY ? HTTP_HARD_MAX_BODY : (size_t)m;
        }
        else if (!lua_isnil(L, -1))
        {
            snprintf(err, errn, "max_body must be a number (bytes)");
            goto out;
        }
        lua_getfield(L, opts, "redirects");
        if (lua_type(L, -1) == LUA_TNUMBER)
        {
            lua_Number m = lua_tonumber(L, -1);
            r->max_redirects = m < 0 ? 0 : m > HTTP_MAX_REDIRECTS ? HTTP_MAX_REDIRECTS : (int)m;
        }
        else if (!lua_isnil(L, -1))
        {
            snprintf(err, errn, "redirects must be a number");
            goto out;
        }
        lua_getfield(L, opts, "ca_cert");
        if (lua_type(L, -1) == LUA_TSTRING)
        {
            size_t cl;
            const char *pem = lua_tolstring(L, -1, &cl);
            if (!strstr(pem, "-----BEGIN"))
            {
                snprintf(err, errn, "ca_cert must be a PEM string");
                goto out;
            }
            r->ca_cert = dup_n(pem, cl);
            if (!r->ca_cert)
            {
                snprintf(err, errn, "out of memory");
                goto out;
            }
            r->ca_cert_len = cl + 1; /* esp_http_client wants the length including the terminating NUL */
        }
        else if (!lua_isnil(L, -1))
        {
            snprintf(err, errn, "ca_cert must be a string");
            goto out;
        }
    }

    /* url */
    if (!url || !(has_prefix_ci(url, "http://") || has_prefix_ci(url, "https://")))
    {
        snprintf(err, errn, "url must be a string starting with http:// or https://");
        goto out;
    }
    if (url_len > HTTP_MAX_URL || strlen(url) != url_len)
    {
        snprintf(err, errn, "url is too long or contains a NUL byte");
        goto out;
    }
    for (size_t i = 0; i < url_len; i++)
        if ((unsigned char)url[i] <= 0x20 || (unsigned char)url[i] == 0x7f)
        {
            snprintf(err, errn, "url must not contain spaces or control characters (percent-encode them)");
            goto out;
        }
    r->url = dup_n(url, url_len);

    /* method */
    {
        static const struct { const char *name; esp_http_client_method_t m; } methods[] = {
            {"GET", HTTP_METHOD_GET}, {"POST", HTTP_METHOD_POST}, {"PUT", HTTP_METHOD_PUT},
            {"PATCH", HTTP_METHOD_PATCH}, {"DELETE", HTTP_METHOD_DELETE}, {"HEAD", HTTP_METHOD_HEAD}};
        bool found = false;
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
            if (strcasecmp(method, methods[i].name) == 0)
            {
                r->method = methods[i].m;
                found = true;
            }
        if (!found)
        {
            snprintf(err, errn, "unsupported method \"%s\" (GET POST PUT PATCH DELETE HEAD)", method);
            goto out;
        }
    }

    /* body */
    if (have_body && body_len > 0)
    {
        if (r->method == HTTP_METHOD_GET || r->method == HTTP_METHOD_HEAD)
        {
            snprintf(err, errn, "GET/HEAD requests cannot have a body");
            goto out;
        }
        if (body_len > HTTP_MAX_REQ_BODY)
        {
            snprintf(err, errn, "request body too large (max %d bytes)", HTTP_MAX_REQ_BODY);
            goto out;
        }
        r->body = dup_n(body, body_len);
        r->body_len = body_len;
        if (!r->body)
        {
            snprintf(err, errn, "out of memory");
            goto out;
        }
    }

    /* headers = { name = value } */
    if (opts)
    {
        lua_getfield(L, opts, "headers");
        if (lua_type(L, -1) == LUA_TTABLE)
        {
            int ht = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, ht))
            {
                if (lua_type(L, -2) != LUA_TSTRING)
                {
                    snprintf(err, errn, "header names must be strings");
                    goto out;
                }
                size_t nl, vl;
                const char *name = lua_tolstring(L, -2, &nl);
                int vt = lua_type(L, -1);
                if (vt != LUA_TSTRING && vt != LUA_TNUMBER)
                {
                    snprintf(err, errn, "header \"%s\": value must be a string or number", name);
                    goto out;
                }
                const char *value = lua_tolstring(L, -1, &vl); /* converts only this stack copy */
                if (!valid_header_name(name, nl) || strlen(name) != nl)
                {
                    snprintf(err, errn, "invalid header name \"%s\"", name);
                    goto out;
                }
                if (memchr(value, '\r', vl) || memchr(value, '\n', vl) || memchr(value, 0, vl))
                {
                    snprintf(err, errn, "header \"%s\": value must not contain CR, LF or NUL", name);
                    goto out;
                }
                if (header_is_managed(name))
                {
                    snprintf(err, errn, "header \"%s\" is set automatically", name);
                    goto out;
                }
                if (r->n_hdr >= HTTP_MAX_REQ_HEADERS)
                {
                    snprintf(err, errn, "too many request headers (max %d)", HTTP_MAX_REQ_HEADERS);
                    goto out;
                }
                r->hdr[r->n_hdr].name = dup_n(name, nl);
                r->hdr[r->n_hdr].value = dup_n(value, vl);
                if (!r->hdr[r->n_hdr].name || !r->hdr[r->n_hdr].value)
                {
                    free(r->hdr[r->n_hdr].name);
                    free(r->hdr[r->n_hdr].value);
                    snprintf(err, errn, "out of memory");
                    goto out;
                }
                r->n_hdr++;
                lua_pop(L, 1); /* value; key stays for lua_next */
            }
        }
        else if (!lua_isnil(L, -1))
        {
            snprintf(err, errn, "headers must be a table");
            goto out;
        }
    }
    ok = true;

out:
    lua_settop(L, top);
    if (!ok)
        req_free_params(r);
    return ok;
}

/* ------------------------------------------------------------------ Lua functions */

/* Queue a request. Returns the id, or 0 and fills err. Takes ownership of *tmp on success. */
static uint32_t enqueue(http_req_t *tmp, char *err, size_t errn)
{
    wifi_core_sta_info_t sta = wifi_core_sta_get_info();
    if (!sta.has_ip)
    {
        snprintf(err, errn, "no network: STA has no IP (wifi.sta.connect first)");
        req_free_params(tmp);
        return 0;
    }
    if (!http_init_once())
    {
        snprintf(err, errn, "cannot start the http task (out of memory)");
        req_free_params(tmp);
        return 0;
    }

    uint32_t id = 0;
    int idx = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < HTTP_MAX_SLOTS; i++)
        if (s_req[i].state == S_FREE)
        {
            idx = i;
            break;
        }
    if (idx >= 0)
    {
        s_req[idx] = *tmp;
        s_req[idx].state = S_QUEUED;
        s_req[idx].id = id = s_next_id++;
        if (s_next_id == 0)
            s_next_id = 1;
    }
    xSemaphoreGive(s_lock);

    if (idx < 0)
    {
        snprintf(err, errn, "too many outstanding requests (max %d): poll() or cancel() some first", HTTP_MAX_SLOTS);
        req_free_params(tmp);
        return 0;
    }
    uint8_t job = (uint8_t)idx;
    xQueueSend(s_jobq, &job, 0); /* capacity == slot count, cannot overflow */
    return id;
}

static int push_result(lua_State *L, http_res_t *res)
{
    if (!res)
    {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }
    if (res->failed)
    {
        lua_pushnil(L);
        lua_pushstring(L, res->err);
        res_free(res);
        return 2;
    }
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, res->status);
    lua_setfield(L, -2, "status");
    lua_pushboolean(L, res->status >= 200 && res->status < 300);
    lua_setfield(L, -2, "ok");
    lua_pushlstring(L, res->body ? (const char *)res->body : "", res->body_len);
    lua_setfield(L, -2, "body");
    lua_pushstring(L, res->final_url ? res->final_url : "");
    lua_setfield(L, -2, "url");
    lua_pushinteger(L, res->redirects);
    lua_setfield(L, -2, "redirects");

    lua_createtable(L, 0, res->n_hdr);
    const char *p = res->hdr;
    for (int i = 0; i < res->n_hdr; i++)
    {
        const char *name = p;
        const char *value = name + strlen(name) + 1;
        p = value + strlen(value) + 1;
        lua_getfield(L, -1, name);
        if (lua_type(L, -1) == LUA_TSTRING) /* repeated header: join like RFC 9110 */
        {
            lua_pushstring(L, ", ");
            lua_pushstring(L, value);
            lua_concat(L, 3);
        }
        else
        {
            lua_pop(L, 1);
            lua_pushstring(L, value);
        }
        lua_setfield(L, -2, name);
    }
    lua_setfield(L, -2, "headers");
    res_free(res);
    return 1;
}

/* timeout_ms: 0 = check once, < 0 = wait forever, > 0 = wait at most that long */
static int poll_id(lua_State *L, uint32_t id, double timeout_ms)
{
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000;
    const int64_t deadline = timeout_ms > 0 ? esp_timer_get_time() + (int64_t)(timeout_ms * 1000.0) : 0;
    for (;;)
    {
        http_res_t *res = NULL;
        bool found = false, done = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < HTTP_MAX_SLOTS; i++)
            if (s_req[i].state != S_FREE && s_req[i].id == id && !s_req[i].cancel)
            {
                found = true;
                if (s_req[i].state == S_DONE)
                {
                    done = true;
                    res = s_req[i].res; /* take the result, free the slot */
                    s_req[i].res = NULL;
                    slot_clear(&s_req[i]);
                }
                break;
            }
        xSemaphoreGive(s_lock);

        if (!found)
            return luaL_error(L, "wifi.http.poll: unknown request id %d (already collected or cancelled)", (int)id);
        if (done)
            return push_result(L, res);

        TickType_t wait;
        if (timeout_ms < 0)
            wait = portMAX_DELAY;
        else
        {
            int64_t rem = deadline - esp_timer_get_time();
            if (timeout_ms == 0 || rem <= 0)
            {
                lua_pushboolean(L, 0); /* still running */
                return 1;
            }
            wait = (TickType_t)((rem + tick_us - 1) / tick_us);
        }
        uint8_t sig;
        if (xQueueReceive(s_doneq, &sig, wait) != pdPASS && timeout_ms < 0)
        {
            lua_pushboolean(L, 0); /* interrupted wait (emulator reset/quit); never happens on the device */
            return 1;
        }
    }
}

static void raise_or_return_error(lua_State *L, const char *err)
{
    lua_pushnil(L);
    lua_pushstring(L, err);
}

/* common front half of request / get / post / start; returns id or 0 (err filled) */
static uint32_t begin_request(lua_State *L, int opts, const char *url, const char *method, const char *body,
                              size_t body_len, bool body_given, char *err, size_t errn)
{
    http_req_t tmp;
    if (!parse_opts(L, opts, url, method, body, body_len, body_given, &tmp, err, errn))
        return 0;
    return enqueue(&tmp, err, errn);
}

static int l_start(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    char err[128];
    uint32_t id = begin_request(L, 1, NULL, NULL, NULL, 0, false, err, sizeof(err));
    if (!id)
    {
        raise_or_return_error(L, err);
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int sync_call(lua_State *L, int opts, const char *url, const char *method, const char *body, size_t body_len,
                     bool body_given)
{
    char err[128];
    uint32_t id = begin_request(L, opts, url, method, body, body_len, body_given, err, sizeof(err));
    if (!id)
    {
        raise_or_return_error(L, err);
        return 2;
    }
    return poll_id(L, id, -1.0);
}

static int l_request(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    return sync_call(L, 1, NULL, NULL, NULL, 0, false);
}

static int l_get(lua_State *L)
{
    size_t ul;
    const char *url = luaL_checklstring(L, 1, &ul);
    int opts = 0;
    if (!lua_isnoneornil(L, 2))
    {
        luaL_checktype(L, 2, LUA_TTABLE);
        opts = 2;
    }
    return sync_call(L, opts, url, "GET", NULL, 0, false);
}

static int l_post(lua_State *L)
{
    size_t ul, bl;
    const char *url = luaL_checklstring(L, 1, &ul);
    const char *body = luaL_checklstring(L, 2, &bl);
    int opts = 0;
    if (!lua_isnoneornil(L, 3))
    {
        luaL_checktype(L, 3, LUA_TTABLE);
        opts = 3;
    }
    return sync_call(L, opts, url, "POST", body, bl, true);
}

static int l_poll(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    lua_Number tmo = luaL_optnumber(L, 2, 0);
    if (!s_lock)
        return luaL_error(L, "wifi.http.poll: unknown request id %d", (int)id);
    return poll_id(L, (uint32_t)id, (double)tmo);
}

static int l_cancel(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    bool existed = false;
    if (s_lock)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < HTTP_MAX_SLOTS; i++)
        {
            http_req_t *r = &s_req[i];
            if (r->state == S_FREE || r->id != (uint32_t)id || r->cancel)
                continue;
            existed = true;
            if (r->state == S_DONE)
                slot_clear(r);
            else
                r->cancel = true; /* queued or running: the worker aborts and frees the slot */
            break;
        }
        xSemaphoreGive(s_lock);
    }
    lua_pushboolean(L, existed);
    return 1;
}

void wifi_http_register(lua_State *L)
{
    http_vm_reset();
    lua_getglobal(L, "wifi");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "wifi");
    }
    static const luaL_Reg fns[] = {{"request", l_request}, {"get", l_get},   {"post", l_post},
                                   {"start", l_start},     {"poll", l_poll}, {"cancel", l_cancel},
                                   {NULL, NULL}};
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, "http");
    lua_pop(L, 1);
}
