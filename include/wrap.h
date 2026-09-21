#ifndef WRAP_H
#define WRAP_H
#include "esp_err.h"
#include "lua.h"
#include "lauxlib.h"
void API_INIT(lua_State *L);
void wrap_init(lua_State *L);
char *read_file(const char *path);
/* Table */
void wrap_table_start(lua_State *L);
void wrap_add_table(lua_State *L);
void wrap_end_table(lua_State *L, const char *name);

/* Values -> Table */
void wrap_add_function(lua_State *L, const char *name, lua_CFunction function);
void wrap_add_number(lua_State *L, const char *name, lua_Number num);
void wrap_add_bool(lua_State *L, const char *name, int value);
void wrap_add_string(lua_State *L, const char *name, const char *value);

/* Values -> Return Stack */
void wrap_return_function(lua_State *L, lua_CFunction function);
void wrap_return_number(lua_State *L, lua_Number num);
void wrap_return_bool(lua_State *L, int value);
void wrap_return_string(lua_State *L, const char *str);
void wrap_return_nil(lua_State *L);
void debug_dump_tasks(void);
#define EEC ESP_ERROR_CHECK
#endif