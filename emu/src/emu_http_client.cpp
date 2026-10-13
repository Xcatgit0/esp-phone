/*
 * emu_http_client.cpp - esp_http_client on top of libcurl (STREAMING), so the device's own code runs unchanged in
 * the emulator: src/wifi/lua_http.c (wifi.http) and src/audio/audio_source.c (network audio source).
 *
 * One transfer thread per client runs curl_easy_perform(); the body goes through a bounded buffer (256 KiB) so a
 * slow reader applies back-pressure to the network instead of buffering a whole download or an endless radio
 * stream. open() returns once the TCP/TLS connection is up, fetch_headers() once the final response headers are
 * in, read() blocks up to cfg.timeout_ms (then -ESP_ERR_HTTP_EAGAIN), close() aborts the transfer.
 *
 * Differences from the device: TLS trusts the host's CA store (or cert_pem), not the device's certificate bundle;
 * no RAM limits; "ICY 200 OK" (Shoutcast v1) answers are accepted here, the real client may reject them.
 * Without libcurl (EMU_HAVE_CURL undefined) every request fails with a clear message.
 */
#include "emu/emu.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
}
#ifdef EMU_HAVE_CURL
#include <curl/curl.h>
#endif

extern "C" esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }

using Hdrs = std::vector<std::pair<std::string, std::string>>;

struct esp_http_client {
    esp_http_client_config_t cfg{};
    std::string url, userAgent, cert;
    Hdrs reqHeaders;
    std::string reqBody;
    bool opened = false;

    // transfer thread + shared state (guarded by m)
    std::thread th;
    bool started = false;
    std::mutex m;
    std::condition_variable cv;
    bool connected = false, headersDone = false, finished = false, abortReq = false, eventsSent = false;
    int curlRc = 0, status = 0;
    long long clen = -1;
    bool chunked = false;
    std::string buf;
    size_t rpos = 0;
    Hdrs resHeaders;
    int sock = -1;
};

#ifdef EMU_HAVE_CURL
namespace {
constexpr size_t kMaxBuffered = 256 * 1024;
std::once_flag g_curlInit;

std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); }); return s; }

size_t onHeader(char *b, size_t sz, size_t n, void *ud) {
    auto *c = (esp_http_client *)ud;
    size_t len = sz * n;
    std::string line(b, len);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    std::lock_guard<std::mutex> l(c->m);
    if (line.rfind("HTTP/", 0) == 0 || line.rfind("ICY ", 0) == 0) {      // a new response block (1xx, redirect, final)
        c->resHeaders.clear();
        auto sp = line.find(' ');
        c->status = sp == std::string::npos ? 0 : atoi(line.c_str() + sp + 1);
        c->clen = -1; c->chunked = false;
        return len;
    }
    if (line.empty()) {                                                      // end of a header block
        if (c->status >= 200 && !c->headersDone) {
            for (auto &kv : c->resHeaders) {
                std::string k = lower(kv.first);
                if (k == "content-length") c->clen = atoll(kv.second.c_str());
                if (k == "transfer-encoding" && lower(kv.second).find("chunked") != std::string::npos) c->chunked = true;
            }
            c->headersDone = true;
            c->cv.notify_all();
        }
        return len;
    }
    auto p = line.find(':');
    if (p == std::string::npos) return len;
    std::string v = line.substr(p + 1);
    v.erase(0, v.find_first_not_of(" \t"));
    c->resHeaders.emplace_back(line.substr(0, p), v);
    return len;
}

size_t onData(char *b, size_t sz, size_t n, void *ud) {
    auto *c = (esp_http_client *)ud;
    size_t len = sz * n;
    std::unique_lock<std::mutex> l(c->m);
    c->cv.wait(l, [&] { return c->abortReq || c->buf.size() - c->rpos < kMaxBuffered; });   // back-pressure
    if (c->abortReq) return 0;
    c->buf.append(b, len);
    c->cv.notify_all();
    return len;
}

int onProgress(void *ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto *c = (esp_http_client *)ud;
    std::lock_guard<std::mutex> l(c->m);
    return c->abortReq ? 1 : 0;
}

curl_socket_t onOpenSocket(void *ud, curlsocktype, curl_sockaddr *addr) {
    auto *c = (esp_http_client *)ud;
    int fd = (int)socket(addr->family, addr->socktype, addr->protocol);
    if (fd >= 0) { std::lock_guard<std::mutex> l(c->m); c->sock = fd; }
    return fd;
}
int onCloseSocket(void *ud, curl_socket_t fd) {
    auto *c = (esp_http_client *)ud;
    { std::lock_guard<std::mutex> l(c->m); if (c->sock == (int)fd) c->sock = -1; }
    close((int)fd);
    return 0;
}
#if LIBCURL_VERSION_NUM >= 0x075000
int onPreReq(void *ud, char *, char *, int, int) {      // connection (and TLS) established, request about to be sent
    auto *c = (esp_http_client *)ud;
    std::lock_guard<std::mutex> l(c->m);
    c->connected = true;
    c->cv.notify_all();
    return CURL_PREREQFUNC_OK;
}
#endif

void transfer(esp_http_client *c) {
    std::call_once(g_curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL *h = curl_easy_init();
    CURLcode rc = CURLE_FAILED_INIT;
    curl_slist *hl = nullptr, *icy = nullptr;
    if (h) {
        curl_easy_setopt(h, CURLOPT_URL, c->url.c_str());
        curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_USERAGENT, c->userAgent.c_str());
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, (long)(c->cfg.timeout_ms > 0 ? c->cfg.timeout_ms : 5000));
        switch (c->cfg.method) {
        case HTTP_METHOD_HEAD:   curl_easy_setopt(h, CURLOPT_NOBODY, 1L); break;
        case HTTP_METHOD_POST:   curl_easy_setopt(h, CURLOPT_POST, 1L); break;
        case HTTP_METHOD_PUT:    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PUT"); break;
        case HTTP_METHOD_PATCH:  curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PATCH"); break;
        case HTTP_METHOD_DELETE: curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE"); break;
        default: curl_easy_setopt(h, CURLOPT_HTTPGET, 1L); break;
        }
        if (!c->reqBody.empty() || c->cfg.method == HTTP_METHOD_POST) {
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)c->reqBody.size());
            curl_easy_setopt(h, CURLOPT_COPYPOSTFIELDS, c->reqBody.c_str());
        }
        for (auto &kv : c->reqHeaders) hl = curl_slist_append(hl, (kv.first + ": " + kv.second).c_str());
        hl = curl_slist_append(hl, "Expect:");      // the ESP client never adds Expect/Accept on its own
        hl = curl_slist_append(hl, "Accept:");
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);
        icy = curl_slist_append(icy, "ICY");
        curl_easy_setopt(h, CURLOPT_HTTP200ALIASES, icy);
        if (!c->cert.empty()) {
#if LIBCURL_VERSION_NUM >= 0x074D00
            curl_blob blob{(void *)c->cert.data(), c->cert.size(), CURL_BLOB_COPY};
            curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
#endif
        }
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHeader);   curl_easy_setopt(h, CURLOPT_HEADERDATA, c);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, onData);      curl_easy_setopt(h, CURLOPT_WRITEDATA, c);
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, onProgress); curl_easy_setopt(h, CURLOPT_XFERINFODATA, c);
        curl_easy_setopt(h, CURLOPT_OPENSOCKETFUNCTION, onOpenSocket);   curl_easy_setopt(h, CURLOPT_OPENSOCKETDATA, c);
        curl_easy_setopt(h, CURLOPT_CLOSESOCKETFUNCTION, onCloseSocket); curl_easy_setopt(h, CURLOPT_CLOSESOCKETDATA, c);
#if LIBCURL_VERSION_NUM >= 0x075000
        curl_easy_setopt(h, CURLOPT_PREREQFUNCTION, onPreReq);   curl_easy_setopt(h, CURLOPT_PREREQDATA, c);
#endif
        rc = curl_easy_perform(h);
        if (rc != CURLE_OK && !c->abortReq) emu::log("http: %s -> %s", c->url.c_str(), curl_easy_strerror(rc));
        curl_slist_free_all(hl); curl_slist_free_all(icy);
        curl_easy_cleanup(h);
    }
    std::lock_guard<std::mutex> l(c->m);
    c->curlRc = (int)rc;
    c->finished = true;
    c->cv.notify_all();
}
}  // namespace

static void startTransfer(esp_http_client *c) {
    if (c->started) return;
    c->started = true;
    c->th = std::thread(transfer, c);
}
static void stopTransfer(esp_http_client *c) {
    {
        std::lock_guard<std::mutex> l(c->m);
        c->abortReq = true;
        if (c->sock >= 0) shutdown(c->sock, SHUT_RDWR);      // wake a curl blocked in poll()/recv()
        c->cv.notify_all();
    }
    if (c->th.joinable()) c->th.join();
}
#else
static void startTransfer(esp_http_client *c) {
    emu::log("http: this emulator was built without libcurl (install libcurl4-openssl-dev and re-run cmake)");
    std::lock_guard<std::mutex> l(c->m);
    c->started = true; c->finished = true; c->curlRc = 1;
}
static void stopTransfer(esp_http_client *) {}
#define CURLE_OK 0
#define CURLE_OPERATION_TIMEDOUT 28
#endif

static int timeoutMs(esp_http_client *c) { return c->cfg.timeout_ms > 0 ? c->cfg.timeout_ms : 5000; }

extern "C" {

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    if (!cfg || !cfg->url) return nullptr;
    auto *c = new esp_http_client;
    c->cfg = *cfg;
    c->url = cfg->url;
    c->userAgent = cfg->user_agent ? cfg->user_agent : "ESP32 HTTP Client/1.0";
    if (cfg->cert_pem && cfg->cert_len > 1) c->cert.assign(cfg->cert_pem, cfg->cert_len - 1);
    return c;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) {
    for (auto &kv : c->reqHeaders)
        if (strcasecmp(kv.first.c_str(), k) == 0) { kv.second = v; return ESP_OK; }
    c->reqHeaders.emplace_back(k, v);
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len) {
    c->opened = true;
    if (write_len > 0) return ESP_OK;                // the request body still has to be written: the transfer starts in fetch_headers()
    startTransfer(c);
    std::unique_lock<std::mutex> l(c->m);
    bool done = c->cv.wait_for(l, std::chrono::milliseconds(timeoutMs(c)), [&] { return c->connected || c->finished || c->headersDone; });
    if (!done) return ESP_ERR_HTTP_CONNECTING;       // connect timed out (the real client reports the same)
    if (!c->connected && !c->headersDone) return c->curlRc == CURLE_OPERATION_TIMEDOUT ? ESP_ERR_HTTP_CONNECTING : ESP_ERR_HTTP_CONNECT;
    return ESP_OK;
}

int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len) {
    if (!c->opened || len < 0) return -1;
    c->reqBody.append(buf, (size_t)len);
    return len;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    startTransfer(c);
    {
        std::unique_lock<std::mutex> l(c->m);
        bool done = c->cv.wait_for(l, std::chrono::milliseconds(timeoutMs(c)), [&] { return c->headersDone || c->finished; });
        if (!done) return -ESP_ERR_HTTP_EAGAIN;
        if (!c->headersDone) return c->curlRc == CURLE_OPERATION_TIMEDOUT ? -ESP_ERR_HTTP_EAGAIN : -1;
    }
    if (!c->eventsSent) {
        c->eventsSent = true;
        Hdrs copy;
        { std::lock_guard<std::mutex> l(c->m); copy = c->resHeaders; }
        if (http_event_handle_cb cb = c->cfg.event_handler) {
            for (auto &kv : copy) {
                std::string k = kv.first, v = kv.second;
                esp_http_client_event_t evt{};
                evt.event_id = HTTP_EVENT_ON_HEADER;
                evt.client = c;
                evt.user_data = c->cfg.user_data;
                evt.header_key = &k[0];
                evt.header_value = &v[0];
                cb(&evt);
            }
        }
    }
    std::lock_guard<std::mutex> l(c->m);
    if (c->chunked) return 0;
    return c->clen > 0 ? c->clen : 0;
}

bool esp_http_client_is_chunked_response(esp_http_client_handle_t c) { std::lock_guard<std::mutex> l(c->m); return c->chunked; }
int  esp_http_client_get_status_code(esp_http_client_handle_t c) { std::lock_guard<std::mutex> l(c->m); return c->status; }

int esp_http_client_read(esp_http_client_handle_t c, char *out, int len) {
    std::unique_lock<std::mutex> l(c->m);
    bool ready = c->cv.wait_for(l, std::chrono::milliseconds(timeoutMs(c)), [&] { return c->rpos < c->buf.size() || c->finished; });
    if (c->rpos < c->buf.size()) {
        size_t n = std::min<size_t>((size_t)len, c->buf.size() - c->rpos);
        memcpy(out, c->buf.data() + c->rpos, n);
        c->rpos += n;
        if (c->rpos > 65536) { c->buf.erase(0, c->rpos); c->rpos = 0; }
        c->cv.notify_all();                                  // the transfer thread may be waiting for space
        return (int)n;
    }
    if (!ready) return -ESP_ERR_HTTP_EAGAIN;
    return (c->curlRc == CURLE_OK || c->abortReq) ? 0 : -1;  // finished: clean end of body, or a broken transfer
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
    std::lock_guard<std::mutex> l(c->m);
    return c->finished && c->rpos >= c->buf.size() && c->curlRc == CURLE_OK;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    stopTransfer(c);
    c->opened = false;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    stopTransfer(c);
    delete c;
    return ESP_OK;
}

}  // extern "C"
