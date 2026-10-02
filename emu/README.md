# phone_emu — SDL2 emulator of the ESP32-S3 phone (Lua API level)

Runs **your project's own Lua VM, `api.c`, `wrap.c`, `tft_gfx.c` and `wifi/lua_wifi.c` unmodified**
on a PC. Only the hardware layer underneath them is replaced (`emu/shim` + `emu/src`), so a Lua
script sees the same functions, return values and timing quirks it sees on the device.

```
<root project>/
  src/ include/ data/ ...      <- unchanged, compiled as-is
  emu/
    CMakeLists.txt
    include/emu/emu.h          <- PUBLIC C++ API (hardware injection, display, modules, events)
    src/                       <- hardware model, display+cursor port, wifi mock, Lua runner, SDL UI
    shim/                      <- tiny fake ESP-IDF / FreeRTOS headers
    modules/                   <- drop new Lua APIs here (*.cpp, auto-registered)
    tests/                     <- headless Lua tests (tests/run_tests.sh)
```

## Build & run

```sh
sudo apt install libsdl2-dev cmake g++        # Linux; macOS: brew install sdl2 cmake
cd <root project>/emu && cmake -B build && cmake --build build -j
./build/phone_emu                             # runs /fs/bootstrap.lua  (/fs = <root>/data)
./build/phone_emu --boot ../bootstrap_old.lua # any other boot script
./build/phone_emu --help
```

`/fs/...` paths in Lua (`readfile`, `loadfile`, `dofile`, `gfx.loadfont`) map to `<root>/data/...`.
Nothing in `data/` is copied — edit a Lua file, press **F5**, it reloads.

## Controls

| Input | Effect |
|---|---|
| mouse on LCD (hold) | touch panel: HR2046 raw x/y/z1/z2 generated from the position |
| drag knobs / arrows / WASD | joystick 1 (cursor) / joystick 2 |
| Space / Enter (or buttons) | joystick push buttons (GPIO46 / GPIO14) |
| 1 2 3 (or buttons) | KEY1..KEY3 (GPIO 9/10/11, active low) |
| F5 / F12 / Esc | reset VM+hardware / screenshot / quit |

The side panel shows live ADC values, touch raw values, cursor, GPIO levels, heap.

## How inputs become the real numbers

* **Joysticks**: UI position → 12-bit ADC (`2048 ± 2047·x`, + noise). The *unchanged* cursor logic
  (`>2800` / `<1200` thresholds, `/4095*2-1`) turns them into `pri_x/pri_y/sec_x/sec_y`.
  Orientation flags: `--set j1-invert-x=1|0`, `j1-invert-y`, `j2-invert-x`, `j2-invert-y`
  (defaults make "stick right" move the cursor right; the device's `cursor.raw(1)` x is then **negative**).
* **Touch**: screen px → raw via bilinear interpolation between the four corner medians taken from
  `data/touch.lua` SAMPLES (x, y, z1, z2), + gaussian noise (`--set touch-noise=6`).
  Released: z1 = 0 (< `Z1_PRESS_MIN`). `tests/touch_roundtrip.lua` checks `touch.lua` maps it back to ≤ 1 px.
* **Buttons / GPIO**: `gpio_get_level()` reads an emulated pin table (idle = 1, pressed = 0).
* **Time**: FreeRTOS tick = 10 ms. `delay(5)` is 0 ticks → no wait, `delay(25)` = 20 ms, `time.millis()`
  has 10 ms resolution — exactly like the device.
* **Heap**: `memory.free()/used()` = configured heap (`--heap-kb`, default 8 MB PSRAM + ~300 KB)
  minus live Lua + C allocations + the 150 KB framebuffer. `--heap-limit-kb N` makes allocations
  beyond N fail, so you can reproduce "not enough memory" errors.

## Faithfully reproduced device quirks (found while porting; worth knowing)

1. `cursor_task` sleeps `pdMS_TO_TICKS(16)` = **1 tick = 10 ms** → cursor speed is 100 px/s, not 62.
2. `queue_task` waits on each queue with a timeout of **16 ticks = 160 ms** (the `16` is ticks, not ms).
   A push that arrives while it is waiting on `drawCursorQueue` is delayed up to ~160 ms
   (`tests/push_latency.lua`: sporadic pushes avg ≈ 30 ms, worst ≈ 160 ms; steady 50 fps streams are fine).
3. Cursor clamp is `0..LCD_WIDTH` / `0..LCD_HEIGHT` **inclusive** (320/240), so at the right/bottom edge
   `draw_cursor()` reads past the framebuffer. The emulator pads the buffer (zeros) instead of corrupting the heap.
4. `main.c` ignores the status of `luaL_loadbuffer`: a **syntax error in bootstrap.lua** shows up as
   `attempt to call a string value`. The emulator prints the real compile error as a `[emu] NOTE` line.
5. `api_init_button` calls `gpio_pullup_dis()` on KEY1..3, so the buttons rely on external pull-ups (the emulator models released = high).
   `api.c` also passes `GPIO_PULLUP_ENABLE` (a `gpio_pullup_t`) where `gpio_pull_mode_t` is expected
   (it happens to end in the right state).
6. Lua gets only `base, table, string, math` — no `io/os/coroutine/utf8/package` (`tests/smoke.lua` asserts this).

## Differences from hardware

* `esp_lcd_panel_draw_bitmap` is clipped to the panel (device may wrap or error).
* WiFi is a mock of `wifi_core.c`: same return codes/state machine, no radio. The air contains one open network
  `emu` (add more with `emu.wifi_add_network(ssid, auth, rssi, channel, password)`). STA connect completes after
  400 ms with IP 10.0.2.15. `wifi.net.request` does a real TCP connect on the host. DNS server = in-memory tables.
  HTTP server endpoints are accepted but not served.
* No SPI/PSRAM/DMA timing, no brown-outs, no task stack limits.

## cursor.poll (device API, also runs in the emulator)

```lua
local id, pressed = cursor.poll([ids [, timeout_ms]])   -- nil on timeout
-- ids: nil = all five | 0..4 | {0,2,3}
-- 0 = joy1 button (GPIO46)  1 = joy2 button (GPIO14)  2 = KEY1 (GPIO9)  3 = KEY2 (GPIO10)  4 = KEY3 (GPIO11)
-- pressed: true = now pressed, false = now released
```

The task sleeps on a queue fed by a GPIO interrupt (no busy loop). Only changes **after** the call are
returned: a button already held is the baseline and its release is the event; nothing that happened between
two calls is kept. Edges are debounced by 10 ms. One event per call. Implemented once, in `src/cursor_poll.c`,
and compiled unchanged into the emulator, whose shim fires the same GPIO ISR path when you click a button or call `emu.key(...)`.
Note the numbering difference in tests: `emu.key(1)` is KEY1 (1-based) but poll id 2 is KEY1.

To act on the hardware while Lua is blocked in `cursor.poll`, use `emu.schedule(ms, "key", 1, true)`
(also `"stick_button"`, `"stick"`, `"touch"`, `"gpio"`), or `emu::schedule()` from C++. `emu.cpu_ms()` returns
process CPU time, used by `tests/poll.lua` to prove the wait is not a busy loop.

## Adding your own APIs

**A. Lua API implemented on the host** — create `emu/modules/mything.cpp` (re-run cmake once):

```cpp
#include "emu/emu.h"
static int my_add(lua_State *L) { lua_pushinteger(L, luaL_checkinteger(L,1) + luaL_checkinteger(L,2)); return 1; }
static int open_my(lua_State *L) {
    static const luaL_Reg f[] = {{"add", my_add}, {nullptr, nullptr}};
    luaL_newlib(L, f); return 1;
}
EMU_LUA_MODULE(my, open_my, "my.add(a,b)");     // -> global `my` exists before bootstrap runs
```
See `modules/example_module.cpp`. When the API later exists on the device, move the code into `src/`
(register it in `API_INIT`) and delete the emulator module; Lua code doesn't change.

**B. Another C++ system driving the emulator** — everything in `include/emu/emu.h` is thread-safe:

```cpp
emu::hw::setKey(0, true);                          // KEY1
emu::hw::setStick(emu::Stick::Primary, 1.f, 0.f);  // joystick 1 full right
emu::hw::setTouch(true, 160, 120);                 // finger at screen centre
emu::hw::setGpio(5, 0);                            // any raw GPIO
uint16_t px[emu::kLcdW * emu::kLcdH]; emu::display::copyPanel(px);   // what the LCD shows (RGB565)
emu::onPush([]{ /* a gfx.push() just reached the LCD */ });
emu::onLuaStart([](lua_State *L){ /* add globals, load fixtures */ });
emu::onFrame([](double dt){ /* UI thread ~60 Hz */ });
```

**C. Scripted tests from Lua** (headless: `phone_emu --headless --run test.lua`): the emulator-only
`emu` table (not present on the device):
`emu.key(i,down) emu.stick(i,x,y) emu.stick_button(i,down) emu.touch(down,x,y) emu.gpio(n[,level])
emu.adc() emu.touch_raw() emu.pixel(x,y[,"fb"]) emu.screenshot(path) emu.pushes() emu.cursor()
emu.wait_push(n,ms) emu.sleep(ms) emu.schedule(ms,action,...) emu.cpu_ms() emu.reset() emu.quit() emu.assert(c,msg) emu.wifi_add_network(...)`

`phone_emu --dump-api` prints every global the VM exposes (compare against `api.lua`).

## When you add C code to the device project

New `src/*.c` files that implement Lua APIs: add them to `DEVICE_SRC` in `emu/CMakeLists.txt`.
If they call ESP-IDF functions the shims lack, the compiler/linker tells you which; add a stub in
`emu/shim` (header) and `emu/src/emu_core.cpp` (implementation). Allocation made with `malloc`
in those files is counted in `memory.used()` automatically (`EMU_TRACK_ALLOC`).
