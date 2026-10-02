/*
 * Example of adding a NEW Lua API to the emulator (host side only).
 *
 * Drop any .cpp into emu/modules/ - CMake picks it up (re-run cmake once) and the
 * EMU_LUA_MODULE line below creates the global table `host` before bootstrap.lua runs.
 *
 * Later, when the same API exists on the device, move the implementation into the
 * project's src/ and delete this file - Lua code does not change.
 */
#include "emu/emu.h"
#include <chrono>

static int host_unix_ms(lua_State *L) {
    using namespace std::chrono;
    lua_pushinteger(L, duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    return 1;
}
static int host_stick(lua_State *L) {                       // host.stick() -> x1,y1,x2,y2 (-1..1)
    float x1, y1, x2, y2;
    emu::hw::stick(emu::Stick::Primary, x1, y1);
    emu::hw::stick(emu::Stick::Secondary, x2, y2);
    lua_pushnumber(L, x1); lua_pushnumber(L, y1); lua_pushnumber(L, x2); lua_pushnumber(L, y2);
    return 4;
}
static int open_host(lua_State *L) {
    static const luaL_Reg fns[] = {{"unix_ms", host_unix_ms}, {"stick", host_stick}, {nullptr, nullptr}};
    luaL_newlib(L, fns);
    return 1;
}
EMU_LUA_MODULE(host, open_host, "example: host.unix_ms(), host.stick()");
