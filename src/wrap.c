#include "wrap.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
void wrap_init(lua_State *L)
{
    (void)L;
}

/*
 * เริ่มสร้าง table หลัก
 *
 * Stack:
 *   before: ...
 *   after : ..., table
 */
void wrap_table_start(lua_State *L)
{
    lua_newtable(L);
}

/*
 * เพิ่ม table ซ้อน
 *
 * Stack:
 *   before: ..., parent
 *   after : ..., parent, child
 */
void wrap_add_table(lua_State *L)
{
    lua_newtable(L);
}

/*
 * จบ table ปัจจุบันแล้วใส่เข้า table แม่
 *
 * Stack:
 *   before: ..., parent, child
 *   after : ..., parent
 */
void wrap_end_table(lua_State *L, const char *name)
{
    lua_setfield(L, -2, name);
}

/*
 * เพิ่ม C function ลง table
 */
void wrap_add_function(lua_State *L,
                       const char *name,
                       lua_CFunction function)
{
    lua_pushcfunction(L, function);
    lua_setfield(L, -2, name);
}

/*
 * เพิ่ม number ลง table
 */
void wrap_add_number(lua_State *L,
                     const char *name,
                     lua_Number num)
{
    lua_pushnumber(L, num);
    lua_setfield(L, -2, name);
}

/*
 * เพิ่ม boolean ลง table
 */
void wrap_add_bool(lua_State *L,
                   const char *name,
                   int value)
{
    lua_pushboolean(L, value ? 1 : 0);
    lua_setfield(L, -2, name);
}


/*
 * เพิ่ม string ลง table
 */
void wrap_add_string(lua_State *L,
                     const char *name,
                     const char *value)
{
    lua_pushstring(L, value ? value : "");
    lua_setfield(L, -2, name);
}


/* =========================================================
 * Return helpers
 * ========================================================= */

/*
 * Return C function
 *
 * ใช้:
 *     wrap_return_function(L, my_function);
 *     return 1;
 */
void wrap_return_function(lua_State *L,
                          lua_CFunction function)
{
    lua_pushcfunction(L, function);
}

/*
 * Return number
 *
 * ใช้:
 *     wrap_return_number(L, 123);
 *     return 1;
 */
void wrap_return_number(lua_State *L,
                        lua_Number num)
{
    lua_pushnumber(L, num);
}

/*
 * Return boolean
 *
 * ใช้:
 *     wrap_return_bool(L, 1);
 *     return 1;
 */
void wrap_return_bool(lua_State *L,
                      int value)
{
    lua_pushboolean(L, value ? 1 : 0);
}

/*
 * Return string
 *
 * ใช้:
 *     wrap_return_string(L, "hello");
 *     return 1;
 */
void wrap_return_string(lua_State *L,
                        const char *str)
{
    lua_pushstring(L, str);
}

/*
 * Return nil
 *
 * ใช้:
 *     wrap_return_nil(L);
 *     return 1;
 */
void wrap_return_nil(lua_State *L)
{
    lua_pushnil(L);
}