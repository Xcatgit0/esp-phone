/*
 * emu.h - public host-side API of the phone emulator.
 *
 * Everything the emulator exposes to *other C++ systems* lives here:
 *   - hw::      inject input (keys, sticks, touch, raw GPIO) and read raw ADC values
 *   - display:: read what the LCD shows, take screenshots
 *   - modules:  add new Lua APIs by dropping a .cpp into emu/modules/ (EMU_LUA_MODULE)
 *   - events:   hook VM start/stop, gfx.push, UI frame ticks
 *   - Lua side: the emulator-only `emu` global (see emu_lua.cpp) for scripted tests
 *
 * All functions are thread-safe unless noted.
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace emu {

constexpr int kLcdW = 320;
constexpr int kLcdH = 240;

enum class Stick { Primary = 0, Secondary = 1 };

/* --------------------------------------------------------------- hardware */
namespace hw {

struct Config {
    float touchNoise   = 6.0f;   // gaussian sigma (raw ADC counts) on touch x/y
    float adcNoise     = 3.0f;   // gaussian sigma on joystick ADC reads
    int   adcCenter    = 2048;
    // Orientation of the emulated sticks relative to the device code.
    // device code: primary x > 2800 => cursor_x--, y > 2800 => cursor_y++
    bool  j1InvertX    = true;   // stick right => ADC low => cursor right
    bool  j1InvertY    = false;
    bool  j2InvertX    = false;
    bool  j2InvertY    = false;
};
Config &config();

/* Buttons KEY1..KEY3 = index 0..2 (GPIO 9/10/11, active low) */
void setKey(int index, bool down);
bool key(int index);
/* x,y in [-1,1]; +x = right, +y = down (screen orientation) */
void setStick(Stick s, float x, float y);
void stick(Stick s, float &x, float &y);
/* Stick push-buttons: primary GPIO46, secondary GPIO14 (active low) */
void setStickButton(Stick s, bool down);
bool stickButton(Stick s);
/* Touch in screen pixels (0..320, 0..240). Converted to HR2046 raw values. */
void setTouch(bool down, float sx = 0, float sy = 0);
bool touchDown();
void touchPos(float &sx, float &sy);
/* Raw GPIO level access (what gpio_get_level() sees). */
void setGpio(int gpio, int level);
int  gpio(int gpio);

struct Adc { int j1x, j1y, j2x, j2y; };      // ADC1 ch2 (GPIO3), ch7 (GPIO8), ADC2 ch1, ch2
Adc  adc();                                   // noiseless, current values
struct TouchRaw { uint16_t x, y, z1, z2; bool pressed; };
TouchRaw touchRaw();                          // noiseless, current values

}  // namespace hw

/* ---------------------------------------------------------------- display */
namespace display {
/* RGB565 copies, kLcdW*kLcdH pixels. panel = LCD memory (includes cursor overlay). */
void     copyPanel(uint16_t *dst);
void     copyFramebuffer(uint16_t *dst);
uint16_t panelPixel(int x, int y);
uint16_t framebufferPixel(int x, int y);
uint32_t pushCount();                         // number of completed tft_push()
struct CursorState { int x, y; bool visible; };
CursorState cursor();
bool     saveBmp(const std::string &path);    // panel as 24-bit BMP
}  // namespace display

/* ------------------------------------------------------------ Lua modules */
/* A module-open function has the luaopen_* shape: push ONE value (usually a
 * table) and return 1. The value is stored in the global `name`.            */
using ModuleOpen = int (*)(lua_State *);
void registerModule(const char *name, ModuleOpen open, const char *doc = "");
struct ModuleRegistrar {
    ModuleRegistrar(const char *n, ModuleOpen o, const char *d = "") { registerModule(n, o, d); }
};
/* Static auto-registration: put this at file scope of any emu/modules/ *.cpp  */
#define EMU_LUA_MODULE(name, open_fn, doc) \
    static ::emu::ModuleRegistrar emu_module_reg_##name(#name, open_fn, doc)

/* ----------------------------------------------------------------- events */
void onLuaStart(std::function<void(lua_State *)> cb);   // after API_INIT, before bootstrap runs
void onLuaStop(std::function<void()> cb);               // VM ended (error, finished, or reset)
void onPush(std::function<void()> cb);                  // after every tft_push() reached the LCD
void onFrame(std::function<void(double dtSeconds)> cb); // UI thread, ~60 Hz (not called headless)

/* ---------------------------------------------------------------- runtime */
/* Run fn on the emulator's scheduler thread after delayMs (real time). Pending actions are dropped on VM reset.
 * Use it to press a button *while* Lua is blocked in cursor.poll(), simulate a delayed touch, etc.            */
void schedule(unsigned delayMs, std::function<void()> fn);
void requestReset();            // restart the Lua VM (like pressing RESET)
void requestQuit();
bool quitRequested();
std::string fsPath(const std::string &devicePath);      // "/fs/x.lua" -> host path
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

struct Options {
    std::string root;            // phone project root (default: parent of emu/)
    std::string fsDir;           // what "/fs" maps to (default: <root>/data)
    std::string bootstrap;       // override boot script (host path); default /fs/bootstrap.lua
    std::string runScript;       // headless test script (host path) run after boot, see `emu` table
    std::string shotPath;        // screenshot on exit
    bool   headless   = false;
    int    scale      = 2;
    size_t heapKb     = 8 * 1024 + 300;   // 8bit-capable heap total (PSRAM + internal)
    size_t psramKb    = 8 * 1024;
    size_t heapLimitKb = 0;      // 0 = no hard limit; else Lua/C allocations beyond this fail (OOM)
    double runSeconds = 0;       // headless: stop after N seconds (0 = until script/VM ends)
    bool   dumpApi    = false;   // print every global exposed to Lua and exit
};
int run(const Options &opt);     // used by main(); blocks until quit

}  // namespace emu
