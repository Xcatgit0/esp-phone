/*
 * emu_core.cpp - FreeRTOS / ESP-IDF shims, heap model, GPIO + ADC + HR2046 hardware model.
 */
#include "emu_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <vector>
#include <cstdarg>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <random>
#include <thread>

extern "C" {
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "hr2046.h"
}

using namespace std::chrono;
namespace emu {
namespace internal {
std::atomic<bool> g_quit{false};
std::atomic<bool> g_reset{false};
thread_local bool t_isLua = false;
Options g_opt;
static const auto g_t0 = steady_clock::now();
}  // namespace internal
using namespace internal;

void requestReset() { g_reset = true; }
void requestQuit()  { g_quit = true; }
bool quitRequested() { return g_quit; }

void log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[emu] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

std::string fsPath(const std::string &p) {
    if (p.rfind("/fs/", 0) == 0) return g_opt.fsDir + p.substr(3);
    if (p == "/fs")             return g_opt.fsDir;
    return p;
}

/* ------------------------------------------------------------------ heap */
namespace internal {
static std::atomic<long> g_used{0};
static size_t g_totalKb = 8 * 1024 + 300, g_psramKb = 8 * 1024, g_limitKb = 0;
void   heapConfigure(size_t t, size_t p, size_t l) { g_totalKb = t; g_psramKb = p; g_limitKb = l; }
size_t heapUsed() { long u = g_used; return u < 0 ? 0 : (size_t)u; }
void   heapCharge(long d) { g_used += d; }
static bool overLimit(size_t extra) {
    return g_limitKb && (size_t)g_used + extra > g_limitKb * 1024;
}
void *luaAlloc(void *, void *ptr, size_t osize, size_t nsize) {
    if (nsize == 0) {
        if (ptr) g_used -= (long)osize;
        free(ptr);
        return nullptr;
    }
    size_t old = ptr ? osize : 0;
    if (nsize > old && overLimit(nsize - old)) return nullptr;   // => "not enough memory"
    void *n = realloc(ptr, nsize);
    if (n) g_used += (long)nsize - (long)old;
    return n;
}
}  // namespace internal
}  // namespace emu

using namespace emu;
using namespace emu::internal;

extern "C" {

void *emu_malloc(size_t n) {
    if (overLimit(n)) return nullptr;
    void *p = malloc(n);
    if (p) g_used += (long)malloc_usable_size(p);
    return p;
}
void *emu_calloc(size_t n, size_t m) {
    if (overLimit(n * m)) return nullptr;
    void *p = calloc(n, m);
    if (p) g_used += (long)malloc_usable_size(p);
    return p;
}
void *emu_realloc(void *p, size_t n) {
    long old = p ? (long)malloc_usable_size(p) : 0;
    if (n == 0) { if (p) { g_used -= old; free(p); } return nullptr; }
    if ((long)n > old && overLimit(n - old)) return nullptr;
    void *r = realloc(p, n);
    if (r) g_used += (long)malloc_usable_size(r) - old;
    return r;
}
void emu_free(void *p) {
    if (!p) return;
    g_used -= (long)malloc_usable_size(p);
    free(p);
}

FILE *emu_fopen(const char *path, const char *mode) {
    return fopen(fsPath(path).c_str(), mode);
}
FILE *emu_freopen(const char *path, const char *mode, FILE *f) {
    return freopen(fsPath(path).c_str(), mode, f);
}

size_t heap_caps_get_total_size(uint32_t caps) {
    size_t psram = g_psramKb * 1024, total = g_totalKb * 1024;
    if (caps & MALLOC_CAP_SPIRAM)   return psram;
    if (caps & MALLOC_CAP_INTERNAL) return total > psram ? total - psram : 0;
    return total;
}
size_t heap_caps_get_free_size(uint32_t caps) {
    size_t t = heap_caps_get_total_size(caps), u = heapUsed();
    if (caps & MALLOC_CAP_INTERNAL) u = u > g_psramKb * 1024 ? 0 : 0;   // internal untouched by our big allocs
    return t > u ? t - u : 0;
}
void *heap_caps_malloc(size_t size, uint32_t) { return emu_malloc(size); }
uint32_t esp_get_free_heap_size(void) {
    size_t t = g_totalKb * 1024, u = heapUsed();
    return (uint32_t)(t > u ? t - u : 0);
}
void *pvPortMalloc(size_t n) { return emu_malloc(n); }
void  vPortFree(void *p)     { emu_free(p); }

const char *esp_err_to_name(esp_err_t c) {
    switch (c) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_WIFI_NOT_STARTED: return "ESP_ERR_WIFI_NOT_STARTED";
    case ESP_ERR_WIFI_NOT_CONNECT: return "ESP_ERR_WIFI_NOT_CONNECT";
    default: return "ESP_ERR_UNKNOWN";
    }
}

void esp_restart(void) {
    emu::log("esp_restart() -> resetting Lua VM");
    requestReset();
    for (;;) std::this_thread::sleep_for(seconds(1));
}
int64_t esp_timer_get_time(void) {
    return duration_cast<microseconds>(steady_clock::now() - g_t0).count();
}

/* ------------------------------------------------------------- FreeRTOS */
TickType_t xTaskGetTickCount(void) {
    return (TickType_t)(duration_cast<milliseconds>(steady_clock::now() - g_t0).count() / portTICK_PERIOD_MS);
}
void vTaskDelay(TickType_t ticks) {
    /* Real FreeRTOS: delay of N ticks, tick = 10 ms (CONFIG_FREERTOS_HZ=100).
     * pdMS_TO_TICKS(<10) == 0 => returns immediately (just a yield).          */
    if (ticks == 0) { std::this_thread::yield(); return; }
    auto end = steady_clock::now() + milliseconds((long)ticks * portTICK_PERIOD_MS);
    while (steady_clock::now() < end) {
        if (g_quit || (t_isLua && g_reset)) return;
        auto left = end - steady_clock::now();
        std::this_thread::sleep_for(std::min<steady_clock::duration>(left, milliseconds(4)));
    }
}
UBaseType_t uxTaskGetNumberOfTasks(void) { return 4; }
UBaseType_t uxTaskGetSystemState(TaskStatus_t *a, UBaseType_t len, uint32_t *total) {
    static const struct { const char *n; unsigned p; unsigned long hw; } t[] = {
        {"lua", 5, 5200}, {"cursor", 6, 1300}, {"TFTQueue", 6, 1500}, {"IDLE1", 0, 900}};
    UBaseType_t n = len < 4 ? len : 4;
    for (UBaseType_t i = 0; i < n; i++) {
        a[i].xHandle = (TaskHandle_t)(uintptr_t)(i + 1);
        a[i].pcTaskName = t[i].n;
        a[i].uxCurrentPriority = t[i].p;
        a[i].usStackHighWaterMark = t[i].hw;
        a[i].ulRunTimeCounter = 0;
    }
    if (total) *total = 0;
    return n;
}
BaseType_t xTaskGetCoreID(TaskHandle_t) { return 1; }

/* queues: real FIFOs (cursor.poll uses length 8); length-1 + overwrite is what tft/cursor use */
struct EmuQueue {
    std::mutex m; std::condition_variable cv;
    unsigned cap, itemSize;
    std::deque<std::vector<unsigned char>> items;
};
QueueHandle_t emu_queue_create(unsigned length, unsigned item_size) {
    auto *q = new EmuQueue; q->cap = length ? length : 1; q->itemSize = item_size; return q;
}
static BaseType_t queuePush(EmuQueue *q, const void *item, bool overwrite) {
    {
        std::lock_guard<std::mutex> l(q->m);
        if (q->items.size() >= q->cap) {
            if (!overwrite) return errQUEUE_FULL;
            q->items.pop_front();
        }
        q->items.emplace_back((const unsigned char *)item, (const unsigned char *)item + q->itemSize);
    }
    q->cv.notify_one();
    return pdPASS;
}
BaseType_t emu_queue_overwrite(QueueHandle_t h, const void *item) { return queuePush((EmuQueue *)h, item, true); }
BaseType_t emu_queue_send(QueueHandle_t h, const void *item) { return queuePush((EmuQueue *)h, item, false); }
BaseType_t emu_queue_send_isr(QueueHandle_t h, const void *item, BaseType_t *woken) {
    if (woken) *woken = pdFALSE;
    return queuePush((EmuQueue *)h, item, false);
}
BaseType_t emu_queue_reset(QueueHandle_t h) {
    auto *q = (EmuQueue *)h; std::lock_guard<std::mutex> l(q->m); q->items.clear(); return pdPASS;
}
BaseType_t emu_queue_receive(QueueHandle_t h, void *item, TickType_t ticks) {
    auto *q = (EmuQueue *)h;
    const bool forever = (ticks == portMAX_DELAY);
    const auto end = steady_clock::now() + milliseconds((long)(forever ? 0 : ticks) * portTICK_PERIOD_MS);
    std::unique_lock<std::mutex> l(q->m);
    for (;;) {
        if (!q->items.empty()) {
            memcpy(item, q->items.front().data(), q->itemSize); q->items.pop_front();
            return pdPASS;
        }
        if (ticks == 0 || g_quit || (t_isLua && g_reset)) return pdFAIL;   // reset/quit wakes a blocked Lua task
        auto slice = milliseconds(20);
        if (!forever) {
            auto left = end - steady_clock::now();
            if (left <= steady_clock::duration::zero()) return pdFAIL;
            if (left < slice) slice = duration_cast<milliseconds>(left) + milliseconds(1);
        }
        q->cv.wait_for(l, slice);
    }
}

/* ----------------------------------------------------------------- GPIO */
static std::atomic<int> g_gpio[64];
static struct GpioInit { GpioInit() { for (auto &g : g_gpio) g = 1; } } g_gpioInit;  // external pull-ups: released = high

/* GPIO interrupts: gpio_set_intr_type + gpio_isr_handler_add, fired from whichever thread changes the level */
struct IsrSlot { gpio_isr_t fn = nullptr; void *arg = nullptr; int type = GPIO_INTR_DISABLE; bool enabled = false; };
static IsrSlot g_isr[64];
static std::mutex g_isrM;

static void gpioWrite(int n, int level) {
    if (n < 0 || n >= 64) return;
    int old = g_gpio[n].exchange(level);
    if (old == level) return;
    gpio_isr_t fn = nullptr; void *arg = nullptr;
    {
        std::lock_guard<std::mutex> l(g_isrM);
        const IsrSlot &s = g_isr[n];
        bool hit = s.enabled && s.fn &&
                   (s.type == GPIO_INTR_ANYEDGE || (s.type == GPIO_INTR_POSEDGE && level == 1) ||
                    (s.type == GPIO_INTR_NEGEDGE && level == 0) ||
                    (s.type == GPIO_INTR_HIGH_LEVEL && level == 1) || (s.type == GPIO_INTR_LOW_LEVEL && level == 0));
        if (hit) { fn = s.fn; arg = s.arg; }
    }
    if (fn) fn(arg);                       // "ISR context": must not block (it only posts to a queue)
}

esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t) { return ESP_OK; }
esp_err_t gpio_set_pull_mode(gpio_num_t, gpio_pull_mode_t) { return ESP_OK; }
esp_err_t gpio_pullup_dis(gpio_num_t) { return ESP_OK; }
int gpio_get_level(gpio_num_t n) { return (n >= 0 && n < 64) ? g_gpio[n].load() : 0; }
esp_err_t gpio_install_isr_service(int) {
    static bool installed = false;
    if (installed) return ESP_ERR_INVALID_STATE;       // same as ESP-IDF
    installed = true; return ESP_OK;
}
esp_err_t gpio_set_intr_type(gpio_num_t n, gpio_int_type_t t) {
    if (n < 0 || n >= 64) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> l(g_isrM); g_isr[n].type = t; return ESP_OK;
}
esp_err_t gpio_isr_handler_add(gpio_num_t n, gpio_isr_t fn, void *arg) {
    if (n < 0 || n >= 64) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> l(g_isrM); g_isr[n].fn = fn; g_isr[n].arg = arg; g_isr[n].enabled = true; return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t n) {
    if (n < 0 || n >= 64) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> l(g_isrM); g_isr[n].fn = nullptr; g_isr[n].arg = nullptr; g_isr[n].enabled = false; return ESP_OK;
}
esp_err_t gpio_intr_enable(gpio_num_t n) {
    if (n < 0 || n >= 64) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> l(g_isrM); g_isr[n].enabled = true; return ESP_OK;
}
esp_err_t gpio_intr_disable(gpio_num_t n) {
    if (n < 0 || n >= 64) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> l(g_isrM); g_isr[n].enabled = false; return ESP_OK;
}

/* HR2046 (XPT2046-compatible) */
esp_err_t hr2046_init(void) { return ESP_OK; }
uint16_t hr2046_read_x(void)  { uint16_t x,y,a,b; touchSample(x,y,a,b); return x; }
uint16_t hr2046_read_y(void)  { uint16_t x,y,a,b; touchSample(x,y,a,b); return y; }
uint16_t hr2046_read_z1(void) { uint16_t x,y,a,b; touchSample(x,y,a,b); return a; }
uint16_t hr2046_read_z2(void) { uint16_t x,y,a,b; touchSample(x,y,a,b); return b; }
hr2046_raw_t hr2046_read(void) { hr2046_raw_t r; touchSample(r.x, r.y, r.z1, r.z2); return r; }
void lua_register_touch(struct lua_State *) {}

}  // extern "C"

/* ------------------------------------------------------- hardware model */
namespace emu {
namespace hw {
namespace {
std::mutex g_m;
Config g_cfg;
bool  g_key[3] = {false, false, false};
float g_stick[2][2] = {{0, 0}, {0, 0}};
bool  g_stickBtn[2] = {false, false};
bool  g_touch = false;
float g_tx = 0, g_ty = 0;
const int kKeyGpio[3] = {9, 10, 11};
const int kStickGpio[2] = {46, 14};

/* Measured corner readings from data/touch.lua (median of the calibration samples):
 *            screen      raw x  raw y  z1    z2
 *   TL       (0,0)       276    3775   285   3904
 *   TR       (320,0)     3886   3822   2064  4005
 *   BL       (0,240)     232    279    154   2544
 *   BR       (320,240)   3853   276    1681  3091
 * Raw values are bilinearly interpolated between the corners.                   */
const float kCorner[4][4] = {
    {276, 3775, 285, 3904}, {3886, 3822, 2064, 4005},
    {232, 279, 154, 2544},  {3853, 276, 1681, 3091}};

float noise(float sigma) {
    thread_local std::mt19937 rng{std::random_device{}()};
    if (sigma <= 0) return 0;
    return std::normal_distribution<float>(0.f, sigma)(rng);
}
int clamp12(float v) { return v < 0 ? 0 : v > 4095 ? 4095 : (int)std::lround(v); }
}  // namespace

Config &config() { return g_cfg; }

void setKey(int i, bool down) {
    if (i < 0 || i > 2) return;
    { std::lock_guard<std::mutex> l(g_m); g_key[i] = down; }
    gpioWrite(kKeyGpio[i], down ? 0 : 1);
}
bool key(int i) { std::lock_guard<std::mutex> l(g_m); return i >= 0 && i < 3 && g_key[i]; }
void setStick(Stick s, float x, float y) {
    std::lock_guard<std::mutex> l(g_m);
    g_stick[(int)s][0] = std::max(-1.f, std::min(1.f, x));
    g_stick[(int)s][1] = std::max(-1.f, std::min(1.f, y));
}
void stick(Stick s, float &x, float &y) {
    std::lock_guard<std::mutex> l(g_m); x = g_stick[(int)s][0]; y = g_stick[(int)s][1];
}
void setStickButton(Stick s, bool down) {
    { std::lock_guard<std::mutex> l(g_m); g_stickBtn[(int)s] = down; }
    gpioWrite(kStickGpio[(int)s], down ? 0 : 1);
}
bool stickButton(Stick s) { std::lock_guard<std::mutex> l(g_m); return g_stickBtn[(int)s]; }
void setTouch(bool down, float sx, float sy) {
    std::lock_guard<std::mutex> l(g_m); g_touch = down; if (down) { g_tx = sx; g_ty = sy; }
}
bool touchDown() { std::lock_guard<std::mutex> l(g_m); return g_touch; }
void touchPos(float &sx, float &sy) { std::lock_guard<std::mutex> l(g_m); sx = g_tx; sy = g_ty; }
void setGpio(int n, int level) { gpioWrite(n, level ? 1 : 0); }
int  gpio(int n) { return (n >= 0 && n < 64) ? g_gpio[n].load() : 0; }

Adc adc() {
    std::lock_guard<std::mutex> l(g_m);
    const int c = g_cfg.adcCenter;
    auto ax = [&](float v, bool inv) { return clamp12(c + (inv ? -v : v) * 2047.f); };
    Adc a;
    a.j1x = ax(g_stick[0][0], g_cfg.j1InvertX);        // ADC1 ch2  (JOY_X_CHANNEL)
    a.j1y = ax(g_stick[0][1], g_cfg.j1InvertY);        // ADC1 ch7  (JOY_Y_CHANNEL)
    /* secondary: device code swaps axes: sec_x from ADC2 ch2 (raw_y), sec_y from ADC2 ch1 (raw_x) */
    a.j2x = ax(g_stick[1][1], g_cfg.j2InvertY);        // ADC2 ch1  -> raw_x -> sec_y
    a.j2y = ax(g_stick[1][0], g_cfg.j2InvertX);        // ADC2 ch2  -> raw_y -> sec_x
    return a;
}

TouchRaw touchRaw() {
    bool down; float sx, sy;
    { std::lock_guard<std::mutex> l(g_m); down = g_touch; sx = g_tx; sy = g_ty; }
    TouchRaw r{};
    r.pressed = down;
    if (!down) { r.x = 0; r.y = 0; r.z1 = 0; r.z2 = 4095; return r; }
    float u = std::max(0.f, std::min(1.f, sx / kLcdW)), w = std::max(0.f, std::min(1.f, sy / kLcdH));
    float v[4];
    for (int k = 0; k < 4; k++)
        v[k] = kCorner[0][k] * (1 - u) * (1 - w) + kCorner[1][k] * u * (1 - w) +
               kCorner[2][k] * (1 - u) * w       + kCorner[3][k] * u * w;
    r.x = (uint16_t)clamp12(v[0]); r.y = (uint16_t)clamp12(v[1]);
    r.z1 = (uint16_t)clamp12(v[2]); r.z2 = (uint16_t)clamp12(v[3]);
    return r;
}
}  // namespace hw

namespace internal {
int adcRead(int unit, int ch) {
    hw::Adc a = hw::adc();
    int v = 2048;
    if (unit == 1 && ch == 2) v = a.j1x;
    else if (unit == 1 && ch == 7) v = a.j1y;
    else if (unit == 2 && ch == 1) v = a.j2x;
    else if (unit == 2 && ch == 2) v = a.j2y;
    return hw::clamp12(v + hw::noise(hw::g_cfg.adcNoise));
}
void touchSample(uint16_t &x, uint16_t &y, uint16_t &z1, uint16_t &z2) {
    hw::TouchRaw r = hw::touchRaw();
    if (r.pressed) {
        float s = hw::g_cfg.touchNoise;
        r.x  = (uint16_t)hw::clamp12(r.x  + hw::noise(s));
        r.y  = (uint16_t)hw::clamp12(r.y  + hw::noise(s));
        r.z1 = (uint16_t)hw::clamp12(r.z1 + hw::noise(s * 1.5f));
        r.z2 = (uint16_t)hw::clamp12(r.z2 + hw::noise(s * 1.5f));
    }
    x = r.x; y = r.y; z1 = r.z1; z2 = r.z2;
}
}  // namespace internal
}  // namespace emu
