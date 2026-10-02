/*
 * emu_ui.cpp - SDL2 front-end: LCD view, mouse = touch panel, on-screen joysticks + KEY1..3,
 *              keyboard shortcuts, live hardware read-outs.
 *
 *   Mouse on LCD (hold)      touch (HR2046 raw values are generated from the position)
 *   Drag the stick knobs     analog joystick 1 (cursor) / joystick 2
 *   Arrow keys / WASD        joystick 1 / joystick 2 (digital, full deflection)
 *   Space / Enter            joystick 1 / joystick 2 push-button
 *   1 2 3                    KEY1 KEY2 KEY3 (or click the on-screen buttons)
 *   F5 reset VM   F12 screenshot   Esc quit
 */
#include "emu_internal.h"
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "esp_heap_caps.h"
#include "esp_system.h"
}

namespace emu {
using namespace internal;

namespace {
constexpr int kPanelW = 300;

struct Canvas {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    std::vector<uint8_t> font;           // PSF1, 8x16, 256 glyphs
    void init(int W, int H) { w = W; h = H; px.assign((size_t)W * H, 0xFF202226); }
    void rect(int x, int y, int rw, int rh, uint32_t c) {
        for (int j = std::max(0, y); j < std::min(h, y + rh); j++)
            for (int i = std::max(0, x); i < std::min(w, x + rw); i++) px[(size_t)j * w + i] = c;
    }
    void frame(int x, int y, int rw, int rh, uint32_t c) {
        rect(x, y, rw, 1, c); rect(x, y + rh - 1, rw, 1, c); rect(x, y, 1, rh, c); rect(x + rw - 1, y, 1, rh, c);
    }
    void circle(int cx, int cy, int r, uint32_t c, bool fill) {
        for (int j = -r; j <= r; j++)
            for (int i = -r; i <= r; i++) {
                int d2 = i * i + j * j;
                bool on = fill ? d2 <= r * r : (d2 <= r * r && d2 >= (r - 1) * (r - 1));
                int x = cx + i, y = cy + j;
                if (on && x >= 0 && y >= 0 && x < w && y < h) px[(size_t)y * w + x] = c;
            }
    }
    void text(int x, int y, const std::string &s, uint32_t c) {
        if (font.size() < 4 + 256 * 16) return;
        for (char ch : s) {
            const uint8_t *g = &font[4 + (uint8_t)ch * 16];
            for (int j = 0; j < 16; j++)
                for (int i = 0; i < 8; i++)
                    if (g[j] & (0x80 >> i)) {
                        int xx = x + i, yy = y + j;
                        if (xx >= 0 && yy >= 0 && xx < w && yy < h) px[(size_t)yy * w + xx] = c;
                    }
            x += 8;
        }
    }
};

uint32_t rgb565(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

struct StickWidget {
    int cx, cy, r; float x = 0, y = 0; bool drag = false; const char *label;
    bool hit(int mx, int my) const { return (mx - cx) * (mx - cx) + (my - cy) * (my - cy) <= r * r; }
    void setFromMouse(int mx, int my) {
        float dx = (float)(mx - cx) / r, dy = (float)(my - cy) / r, m = std::sqrt(dx * dx + dy * dy);
        if (m > 1) { dx /= m; dy /= m; }
        x = dx; y = dy;
    }
};
struct ButtonWidget { int x, y, w, h; std::string label; bool down = false; bool hit(int mx, int my) const { return mx >= x && my >= y && mx < x + w && my < y + h; } };
}  // namespace

static void saveCanvasBmp(const Canvas &cv, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    uint32_t rowBytes = cv.w * 3, size = 54 + rowBytes * cv.h;
    unsigned char h[54] = {'B', 'M'};
    auto w32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) h[o + i] = (v >> (8 * i)) & 0xFF; };
    w32(2, size); w32(10, 54); w32(14, 40); w32(18, cv.w); w32(22, cv.h); h[26] = 1; h[28] = 24;
    fwrite(h, 1, 54, f);
    for (int y = cv.h - 1; y >= 0; y--)
        for (int x = 0; x < cv.w; x++) {
            uint32_t c = cv.px[(size_t)y * cv.w + x];
            unsigned char bgr[3] = {(unsigned char)(c & 255), (unsigned char)((c >> 8) & 255), (unsigned char)((c >> 16) & 255)};
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
}

static int runGui(const Options &opt) {
    const int S = std::max(1, opt.scale);
    const int lcdW = kLcdW * S, lcdH = kLcdH * S;
    const int W = lcdW + kPanelW, H = std::max(lcdH, 560);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("ESP32-S3 Phone Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);

    Canvas cv; cv.init(W, H);
    if (FILE *f = fopen(fsPath("/fs/font.psf").c_str(), "rb")) {
        cv.font.resize(4 + 256 * 16); size_t n = fread(cv.font.data(), 1, cv.font.size(), f); fclose(f);
        if (n < cv.font.size()) cv.font.clear();
    }

    const int px0 = lcdW + 14;
    StickWidget j1{px0 + 70, 112, 52, 0, 0, false, "JOY1 (cursor)"};
    StickWidget j2{px0 + 70, 262, 52, 0, 0, false, "JOY2"};
    ButtonWidget jb1{px0 + 150, 100, 120, 24, "J1 PRESS"}, jb2{px0 + 150, 250, 120, 24, "J2 PRESS"};
    ButtonWidget kb[3] = {{px0, 336, 80, 32, "KEY1"}, {px0 + 90, 336, 80, 32, "KEY2"}, {px0 + 180, 336, 80, 32, "KEY3"}};

    bool touching = false; int shotN = 0, shotFrames = 0;
    std::vector<uint16_t> panel(kLcdW * kLcdH);
    auto last = std::chrono::steady_clock::now();
    double fpsAcc = 0; int fpsN = 0; double fps = 0;

    vmThreadStart();

    auto lcdPos = [&](int mx, int my, float &sx, float &sy) {
        sx = std::max(0.f, std::min((float)kLcdW, (float)mx / S));
        sy = std::max(0.f, std::min((float)kLcdH, (float)my / S));
    };

    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) requestQuit();
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: requestQuit(); break;
                case SDLK_F5: requestReset(); break;
                case SDLK_F12: { char n[64]; snprintf(n, sizeof n, "shot_%03d.bmp", shotN++); display::saveBmp(n); emu::log("screenshot -> %s", n); break; }
                case SDLK_1: kb[0].down = true; break;
                case SDLK_2: kb[1].down = true; break;
                case SDLK_3: kb[2].down = true; break;
                case SDLK_SPACE: jb1.down = true; break;
                case SDLK_RETURN: jb2.down = true; break;
                default: break;
                }
            } else if (e.type == SDL_KEYUP) {
                switch (e.key.keysym.sym) {
                case SDLK_1: kb[0].down = false; break;
                case SDLK_2: kb[1].down = false; break;
                case SDLK_3: kb[2].down = false; break;
                case SDLK_SPACE: jb1.down = false; break;
                case SDLK_RETURN: jb2.down = false; break;
                default: break;
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x, my = e.button.y;
                if (mx < lcdW && my < lcdH) { touching = true; float sx, sy; lcdPos(mx, my, sx, sy); hw::setTouch(true, sx, sy); }
                else if (j1.hit(mx, my)) { j1.drag = true; j1.setFromMouse(mx, my); }
                else if (j2.hit(mx, my)) { j2.drag = true; j2.setFromMouse(mx, my); }
                else if (jb1.hit(mx, my)) jb1.down = true;
                else if (jb2.hit(mx, my)) jb2.down = true;
                else for (auto &b : kb) if (b.hit(mx, my)) b.down = true;
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                if (touching) { touching = false; hw::setTouch(false); }
                j1.drag = j2.drag = false; j1.x = j1.y = j2.x = j2.y = 0;
                // mouse-held buttons release; keyboard-held ones are re-asserted below
                for (auto &b : kb) b.down = false;
                jb1.down = jb2.down = false;
            } else if (e.type == SDL_MOUSEMOTION) {
                int mx = e.motion.x, my = e.motion.y;
                if (touching) { float sx, sy; lcdPos(mx, my, sx, sy); hw::setTouch(true, sx, sy); }
                if (j1.drag) j1.setFromMouse(mx, my);
                if (j2.drag) j2.setFromMouse(mx, my);
            }
        }

        /* keyboard state -> digital sticks (only when not dragging) */
        const Uint8 *ks = SDL_GetKeyboardState(nullptr);
        float k1x = (float)(ks[SDL_SCANCODE_RIGHT] - ks[SDL_SCANCODE_LEFT]), k1y = (float)(ks[SDL_SCANCODE_DOWN] - ks[SDL_SCANCODE_UP]);
        float k2x = (float)(ks[SDL_SCANCODE_D] - ks[SDL_SCANCODE_A]), k2y = (float)(ks[SDL_SCANCODE_S] - ks[SDL_SCANCODE_W]);
        float s1x = j1.drag ? j1.x : k1x, s1y = j1.drag ? j1.y : k1y;
        float s2x = j2.drag ? j2.x : k2x, s2y = j2.drag ? j2.y : k2y;
        auto norm = [](float &x, float &y) { float m = std::sqrt(x * x + y * y); if (m > 1) { x /= m; y /= m; } };
        norm(s1x, s1y); norm(s2x, s2y);
        hw::setStick(Stick::Primary, s1x, s1y); hw::setStick(Stick::Secondary, s2x, s2y);
        for (int i = 0; i < 3; i++) hw::setKey(i, kb[i].down || ks[SDL_SCANCODE_1 + i]);
        hw::setStickButton(Stick::Primary, jb1.down || ks[SDL_SCANCODE_SPACE]);
        hw::setStickButton(Stick::Secondary, jb2.down || ks[SDL_SCANCODE_RETURN]);

        /* ---- draw ---- */
        display::copyPanel(panel.data());
        for (int y = 0; y < lcdH; y++)
            for (int x = 0; x < lcdW; x++) cv.px[(size_t)y * W + x] = rgb565(panel[(y / S) * kLcdW + (x / S)]);
        if (lcdH < H) cv.rect(0, lcdH, lcdW, H - lcdH, 0xFF101114);
        if (hw::touchDown()) { float sx, sy; hw::touchPos(sx, sy); cv.circle((int)(sx * S), (int)(sy * S), 6, 0xFFFFD040, false); }
        cv.rect(lcdW, 0, kPanelW, H, 0xFF202226);

        const uint32_t FG = 0xFFDDDDDD, DIM = 0xFF8A8F98, ACC = 0xFF58C4DD, ON = 0xFF58DD7A;
        cv.text(px0, 8, "ESP32-S3 PHONE EMULATOR", ACC);
        for (StickWidget *sw : {&j1, &j2}) {
            cv.circle(sw->cx, sw->cy, sw->r, 0xFF30343A, true); cv.circle(sw->cx, sw->cy, sw->r, DIM, false);
            float x, y; hw::stick(sw == &j1 ? Stick::Primary : Stick::Secondary, x, y);
            cv.circle(sw->cx + (int)(x * (sw->r - 14)), sw->cy + (int)(y * (sw->r - 14)), 14, sw->drag ? ON : ACC, true);
            cv.text(sw->cx - 52, sw->cy - sw->r - 20, sw->label, DIM);
        }
        for (ButtonWidget *b : {&jb1, &jb2, &kb[0], &kb[1], &kb[2]}) {
            cv.rect(b->x, b->y, b->w, b->h, b->down ? ON : 0xFF3A3F47); cv.frame(b->x, b->y, b->w, b->h, DIM);
            cv.text(b->x + (b->w - 8 * (int)b->label.size()) / 2, b->y + (b->h - 16) / 2, b->label, b->down ? 0xFF101010 : FG);
        }

        auto a = hw::adc(); auto tr = hw::touchRaw(); auto cur = display::cursor();
        char buf[96]; int ty = 386;
        snprintf(buf, sizeof buf, "ADC1 x%4d y%4d  ADC2 %4d %4d", a.j1x, a.j1y, a.j2x, a.j2y); cv.text(px0, ty, buf, FG); ty += 18;
        snprintf(buf, sizeof buf, "T:%s x%4u y%4u z1 %4u z2 %4u", tr.pressed ? "ON" : "--", tr.x, tr.y, tr.z1, tr.z2); cv.text(px0, ty, buf, tr.pressed ? ON : FG); ty += 18;
        snprintf(buf, sizeof buf, "cursor %3d,%3d %s", cur.x, cur.y, cur.visible ? "visible" : "hidden"); cv.text(px0, ty, buf, FG); ty += 18;
        snprintf(buf, sizeof buf, "KEYS %d%d%d  J1btn %d  J2btn %d", hw::key(0), hw::key(1), hw::key(2), hw::stickButton(Stick::Primary), hw::stickButton(Stick::Secondary)); cv.text(px0, ty, buf, FG); ty += 18;
        size_t freeB = esp_get_free_heap_size(), totB = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        snprintf(buf, sizeof buf, "heap used %zu KB / %zu KB", (totB - freeB) / 1024, totB / 1024); cv.text(px0, ty, buf, FG); ty += 18;
        snprintf(buf, sizeof buf, "pushes %u  ui %.0f fps  vm %s", display::pushCount(), fps, vmRunning() ? "RUN" : "stopped"); cv.text(px0, ty, buf, vmRunning() ? ON : 0xFFDD7A58); ty += 24;
        cv.text(px0, ty, "arrows/WASD sticks  1 2 3 keys", DIM); ty += 16;
        cv.text(px0, ty, "space/enter stick btn  F5 reset", DIM); ty += 16;
        cv.text(px0, ty, "mouse on LCD = touch  F12 shot", DIM);

        SDL_UpdateTexture(tex, nullptr, cv.px.data(), W * 4);
        SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, nullptr, nullptr); SDL_RenderPresent(ren);

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count(); last = now;
        fpsAcc += dt; fpsN++; if (fpsAcc >= 0.5) { fps = fpsN / fpsAcc; fpsAcc = 0; fpsN = 0; }
        fireFrame(dt);
        if (!SDL_GetWindowFlags(win)) break;
        if (!opt.shotPath.empty() && ++shotFrames == 60) {      // --shot in GUI mode: whole window, then exit
            saveCanvasBmp(cv, opt.shotPath.c_str()); emu::log("window screenshot -> %s", opt.shotPath.c_str()); requestQuit();
        }
    }
    requestQuit();
    vmThreadJoin();
    SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}

int run(const Options &opt) {
    g_opt = opt;
    heapConfigure(opt.heapKb, opt.psramKb, opt.heapLimitKb);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (opt.headless || opt.dumpApi) return runHeadless();
    return runGui(opt);
}
}  // namespace emu
