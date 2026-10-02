/*
 * emu_display.cpp - port of src/tft.c + src/cursor.c on top of an in-memory "LCD".
 *
 * Kept as close to the device code as possible so timing quirks are preserved:
 *   - cursor_task period = pdMS_TO_TICKS(16) = 1 tick = 10 ms  (100 px/s)
 *   - queue_task waits on each queue with a 16-TICK (=160 ms) timeout
 *   - cursor is drawn by XOR-ing the cursor bitmap into LCD memory after every push
 *   - cursor bounds are 0..LCD_WIDTH / 0..LCD_HEIGHT (inclusive), not WIDTH-1
 * Differences from hardware (documented in emu/README.md):
 *   - esp_lcd_panel_draw_bitmap() is clipped to the panel instead of wrapping/erroring
 *   - the device reads past the end of `framebuffer` when the cursor is on the bottom/right edge;
 *     the emulator pads the buffer with zeros instead.
 */
#include "emu_internal.h"
#include <cstring>
#include <mutex>
#include <thread>

extern "C" {
#include "tft.h"
#include "freertos/queue.h"
}
extern "C" {
#include "cursor.h"
}

using namespace emu::internal;

/* ------------------------------------------------------------ globals the project headers declare */
extern "C" {
uint16_t *framebuffer = nullptr;
esp_lcd_panel_handle_t panel = nullptr;
esp_lcd_panel_io_handle_t io_handle = nullptr;
QueueHandle_t pushQueue = nullptr;
SemaphoreHandle_t push_mutex = nullptr;
SemaphoreHandle_t trans_done_sem = nullptr;
QueueHandle_t drawCursorQueue = nullptr;

int cursor_x = 0, cursor_y = 0;
float sec_x = 0.f, sec_y = 0.f, pri_x = 0.f, pri_y = 0.f;
int raw_x = 0, raw_y = 0;
int cursor_visible = 1;
uint8_t isSHOW = 0;
}

namespace {
constexpr size_t kFbBytes = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
constexpr size_t kFbPadRows = 18;                       // cursor reads up to 16 rows (+1) past the bottom
uint16_t g_panelMem[LCD_WIDTH * LCD_HEIGHT];            // what the LCD shows
std::mutex g_pushMutex;                                 // == push_mutex
std::mutex g_panelMutex;                                // protects g_panelMem for the UI thread
std::atomic<uint32_t> g_pushCount{0};
std::atomic<bool> g_run{false};
std::thread g_cursorThread, g_queueThread;
uint16_t g_mouseBox[16 * 16];
uint16_t g_restoreBox[16 * 16];
int g_oldX = 0, g_oldY = 0;
uint8_t g_event = 1;
}

/* esp_lcd_panel_draw_bitmap equivalent, clipped */
static void panel_draw_bitmap(int x0, int y0, int x1, int y1, const uint16_t *src) {
    std::lock_guard<std::mutex> l(g_panelMutex);
    int w = x1 - x0;
    for (int y = y0; y < y1; y++) {
        if (y < 0 || y >= LCD_HEIGHT) continue;
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= LCD_WIDTH) continue;
            g_panelMem[y * LCD_WIDTH + x] = src[(y - y0) * w + (x - x0)];
        }
    }
}

extern "C" {

esp_err_t tft_init(void) {
    framebuffer = (uint16_t *)calloc(1, kFbBytes + kFbPadRows * LCD_WIDTH * sizeof(uint16_t));
    heapCharge((long)kFbBytes);                        // heap_caps_malloc(LCD_FB_SIZE, SPIRAM)
    panel = (esp_lcd_panel_handle_t)(uintptr_t)1;      // non-null "handle"
    io_handle = (esp_lcd_panel_io_handle_t)(uintptr_t)1;
    push_mutex = (SemaphoreHandle_t)&g_pushMutex;
    pushQueue = xQueueCreate(1, sizeof(uint8_t));
    tft_fill(0x0000);
    tft_push();
    return ESP_OK;
}

void tft_fill(uint16_t color) {
    if (!framebuffer) return;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) framebuffer[i] = tft_convert_color(color);
}

void tft_draw_pixel(int x, int y, uint16_t color) {
    if (!framebuffer) return;
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
    framebuffer[y * LCD_WIDTH + x] = tft_convert_color(color);
}

void tft_push(void) {
    {
        std::lock_guard<std::mutex> l(g_pushMutex);
        std::lock_guard<std::mutex> l2(g_panelMutex);
        memcpy(g_panelMem, framebuffer, kFbBytes);
    }
    g_pushCount++;
    if (drawCursorQueue) xQueueOverwrite(drawCursorQueue, &g_event);
    firePush();
}

void tft_test(void) {}

void draw_cursor(void) {
    if (!cursor_visible) return;
    std::lock_guard<std::mutex> l(g_pushMutex);
    if (!framebuffer) return;
    /* 1) restore background at the old position from the framebuffer */
    for (int y = 0; y < 16; y++)
        memcpy(&g_restoreBox[y * 16], &framebuffer[(g_oldY + y) * LCD_WIDTH + g_oldX], 16 * sizeof(uint16_t));
    panel_draw_bitmap(g_oldX, g_oldY, g_oldX + 16, g_oldY + 16, g_restoreBox);
    /* 2) copy the new background, invert the cursor shape, draw it */
    for (int y = 0; y < 16; y++)
        memcpy(&g_mouseBox[y * 16], &framebuffer[(cursor_y + y) * LCD_WIDTH + cursor_x], 16 * sizeof(uint16_t));
    isSHOW = 1;
    for (int y = 0; y < 16; y++) {
        uint16_t row = cursor_window[y];
        for (int x = 0; x < 16; x++)
            if (row & (0x8000 >> x)) g_mouseBox[y * 16 + x] = (uint16_t)~g_mouseBox[y * 16 + x];
    }
    panel_draw_bitmap(cursor_x, cursor_y, cursor_x + 16, cursor_y + 16, g_mouseBox);
    g_oldX = cursor_x;
    g_oldY = cursor_y;
}

}  // extern "C"

/* ------------------------------------------------------------------ tasks */
static void cursor_task() {
    int oldX = -1, oldY = -1;
    while (g_run && !g_quit) {
        int rx = adcRead(2, 1), ry = adcRead(2, 2);
        raw_x = rx; raw_y = ry;
        sec_x = ((float)ry / 4095.0f) * 2.0f - 1.0f;
        sec_y = ((float)rx / 4095.0f) * 2.0f - 1.0f;
        int x = adcRead(1, 2), y = adcRead(1, 7);
        pri_x = ((float)x / 4095.0f) * 2.0f - 1.0f;
        pri_y = ((float)y / 4095.0f) * 2.0f - 1.0f;
        if (x > 2800) cursor_x--; else if (x < 1200) cursor_x++;
        if (y > 2800) cursor_y++; else if (y < 1200) cursor_y--;
        if (!(oldX == cursor_x && oldY == cursor_y)) draw_cursor();
        if (cursor_x < 0) cursor_x = 0;
        if (cursor_x > LCD_WIDTH) cursor_x = LCD_WIDTH;
        if (cursor_y < 0) cursor_y = 0;
        if (cursor_y > LCD_HEIGHT) cursor_y = LCD_HEIGHT;
        oldX = cursor_x; oldY = cursor_y;
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

static void queue_task() {
    uint8_t ev;
    while (g_run && !g_quit) {
        if (xQueueReceive(pushQueue, &ev, 16) == pdPASS) tft_push();
        if (xQueueReceive(drawCursorQueue, &ev, 16) == pdPASS) draw_cursor();
    }
}

extern "C" void joystick_init(void) {
    drawCursorQueue = xQueueCreate(1, sizeof(uint8_t));
    isSHOW = 0;
    g_run = true;
    g_cursorThread = std::thread(cursor_task);
    g_queueThread = std::thread(queue_task);
}

namespace emu {
namespace internal {
void displayStart() {
    memset(g_panelMem, 0, sizeof(g_panelMem));
    g_pushCount = 0;
    cursor_x = cursor_y = 0; cursor_visible = 1; g_oldX = g_oldY = 0;
    pri_x = pri_y = sec_x = sec_y = 0;
    tft_init();
    joystick_init();
}
void displayStop() {
    g_run = false;
    if (g_cursorThread.joinable()) g_cursorThread.join();
    if (g_queueThread.joinable()) g_queueThread.join();
    if (framebuffer) { free(framebuffer); framebuffer = nullptr; heapCharge(-(long)kFbBytes); }
    pushQueue = nullptr; drawCursorQueue = nullptr;
}
}  // namespace internal

namespace display {
void copyPanel(uint16_t *dst) { std::lock_guard<std::mutex> l(g_panelMutex); memcpy(dst, g_panelMem, kFbBytes); }
void copyFramebuffer(uint16_t *dst) {
    std::lock_guard<std::mutex> l(g_pushMutex);
    if (framebuffer) memcpy(dst, framebuffer, kFbBytes); else memset(dst, 0, kFbBytes);
}
uint16_t panelPixel(int x, int y) {
    if (x < 0 || y < 0 || x >= kLcdW || y >= kLcdH) return 0;
    std::lock_guard<std::mutex> l(g_panelMutex); return g_panelMem[y * kLcdW + x];
}
uint16_t framebufferPixel(int x, int y) {
    if (x < 0 || y < 0 || x >= kLcdW || y >= kLcdH || !framebuffer) return 0;
    return framebuffer[y * kLcdW + x];
}
uint32_t pushCount() { return g_pushCount; }
CursorState cursor() { return {cursor_x, cursor_y, cursor_visible != 0}; }

bool saveBmp(const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    static uint16_t px[kLcdW * kLcdH];
    copyPanel(px);
    const uint32_t rowBytes = kLcdW * 3, size = 54 + rowBytes * kLcdH;
    unsigned char h[54] = {'B', 'M'};
    auto w32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) h[o + i] = (v >> (8 * i)) & 0xFF; };
    w32(2, size); w32(10, 54); w32(14, 40); w32(18, kLcdW); w32(22, kLcdH);
    h[26] = 1; h[28] = 24;
    fwrite(h, 1, 54, f);
    for (int y = kLcdH - 1; y >= 0; y--)
        for (int x = 0; x < kLcdW; x++) {
            uint16_t c = px[y * kLcdW + x];
            unsigned char bgr[3] = {(unsigned char)((c & 0x1F) << 3), (unsigned char)(((c >> 5) & 0x3F) << 2),
                                    (unsigned char)(((c >> 11) & 0x1F) << 3)};
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
    return true;
}
}  // namespace display
}  // namespace emu
