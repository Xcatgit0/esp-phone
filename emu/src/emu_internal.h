/* Private to the emulator implementation. */
#pragma once
#include "emu/emu.h"
#include <atomic>
#include <cstddef>

namespace emu { namespace internal {

extern std::atomic<bool> g_quit;
extern std::atomic<bool> g_reset;
extern thread_local bool t_isLua;     // true on the thread running the Lua VM
extern Options g_opt;

/* heap model (esp_get_free_heap_size, memory.free(), memory.used()) */
void   heapConfigure(size_t totalKb, size_t psramKb, size_t limitKb);
void  *luaAlloc(void *ud, void *ptr, size_t osize, size_t nsize);   // lua_Alloc with accounting + OOM limit
size_t heapUsed();
void   heapCharge(long delta);        // account memory allocated outside malloc (e.g. framebuffer)

/* raw ADC (what adc_oneshot_read() returns) incl. noise; unit 1|2, channel number */
int  adcRead(int unit, int channel);

/* hr2046 raw touch reading incl. noise */
void touchSample(uint16_t &x, uint16_t &y, uint16_t &z1, uint16_t &z2);

/* events / modules (emu_lua.cpp) */
void fireLuaStart(lua_State *L);
void fireLuaStop();
void firePush();
void fireFrame(double dt);
void openModules(lua_State *L);       // registers every EMU_LUA_MODULE + the `emu` table
void dumpApi(lua_State *L);

/* display / cursor (emu_display.cpp) */
void displayStart();                  // tft_init + joystick_init equivalent (starts threads)
void displayStop();

/* wifi mock (emu_wifi.cpp) */
void wifiReset();
void wifiAddNetwork(const char *ssid, const char *auth, int rssi, int channel, const char *password);

/* VM runner (emu_lua.cpp) */
void vmThreadStart();
void vmThreadJoin();
bool vmRunning();
int  runHeadless();

}}  // namespace emu::internal
