#ifndef LUA_WIFI_H
#define LUA_WIFI_H

#include "lua.h"

/* Registers the global "wifi" table (with wifi.sta/.ap/.dhcp/.dns/.net/.signal
 * sub-tables) on the given Lua state. Call once, from API_INIT(). */
void lua_wifi_register(lua_State *L);

#endif /* LUA_WIFI_H */
