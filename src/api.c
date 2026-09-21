#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua.h"
#include "lauxlib.h"
#include "driver/gpio.h"
#include "lualib.h"
#include "tft.h"
#include "cursor.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include "tft_gfx.h"
#include "wrap.h"
#include "lua_wifi.h"
static gfx_font_t *lua_loaded_font = NULL;

static int api_gfx_line(lua_State *L)
{
    gfx_draw_line(luaL_checkinteger(L,1), luaL_checkinteger(L,2),
                  luaL_checkinteger(L,3), luaL_checkinteger(L,4),
                  (uint16_t)luaL_checkinteger(L,5));
    return 0;
}

static int api_gfx_rect(lua_State *L)
{
    int x = luaL_checkinteger(L,1), y = luaL_checkinteger(L,2);
    int w = luaL_checkinteger(L,3), h = luaL_checkinteger(L,4);
    uint16_t color = (uint16_t)luaL_checkinteger(L,5);
    bool filled = lua_toboolean(L,6);

    filled ? gfx_fill_rect(x,y,w,h,color) : gfx_draw_rect(x,y,w,h,color);
    return 0;
}

static int api_gfx_circle(lua_State *L)
{
    int cx = luaL_checkinteger(L,1), cy = luaL_checkinteger(L,2);
    int r  = luaL_checkinteger(L,3);
    uint16_t color = (uint16_t)luaL_checkinteger(L,4);
    bool filled = lua_toboolean(L,5);

    filled ? gfx_fill_circle(cx,cy,r,color) : gfx_draw_circle(cx,cy,r,color);
    return 0;
}

static int api_gfx_loadfont(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    if (lua_loaded_font != NULL) {
        gfx_font_free(lua_loaded_font);
        lua_loaded_font = NULL;
    }

    lua_loaded_font = gfx_font_load(path);
    if (lua_loaded_font == NULL) {
        lua_pushboolean(L, false);
        return 1;
    }

    gfx_set_default_font(lua_loaded_font);
    lua_pushboolean(L, true);
    return 1;
}

static int api_gfx_text(lua_State *L)
{
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    uint16_t fg = (uint16_t)luaL_checkinteger(L, 4);
    uint16_t bg = (uint16_t)luaL_optinteger(L, 5, 0x0000);

    gfx_draw_string(x, y, str, fg, bg, NULL);
    return 0;
}
uint8_t dbuffer = 0;
static int api_gfx_push(lua_State *L)
{   

    //tft_push();
    xQueueOverwrite(pushQueue,&dbuffer);
    return 0;
}
int api_time_millis(lua_State *L)
{
    uint32_t ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    lua_pushinteger(L, ms);

    return 1;
}
int api_memory_free(lua_State *L)
{
    lua_pushinteger(L, esp_get_free_heap_size());

    return 1;
}

int api_memory_used(lua_State *L)
{
    size_t free = esp_get_free_heap_size();
    size_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);

    lua_pushinteger(L, total - free);

    return 1;
}
int api_system_delay(lua_State *L)
{
    int ms = luaL_checkinteger(L, 1);

    vTaskDelay(pdMS_TO_TICKS(ms));

    return 0;
}

char *read_file(const char *path)
{
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        printf("Cannot open: %s\n", path);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char *buffer = malloc(size + 1);

    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    fread(buffer, 1, size, file);
    buffer[size] = '\0';

    fclose(file);

    return buffer;
}
int api_read_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    char *data = read_file(path);

    if (data == NULL) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, data);

    free(data);

    return 1;
}
int api_getcursor(lua_State *L) {
    wrap_return_number(L,cursor_x);
    wrap_return_number(L,cursor_y);
    return 2;
}
int api_joybutton(lua_State *L) {
    wrap_return_number(L,gpio_get_level(GPIO_NUM_46));
    return 1;
}


void debug_dump_tasks(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *list = pvPortMalloc(n * sizeof(TaskStatus_t));
    if (!list) return;

    uint32_t total_runtime;
    n = uxTaskGetSystemState(list, n, &total_runtime);

    printf("%-14s %-6s %-6s %-8s %-6s\n",
           "Name","Prio","Core","StackHW","CPU%%");

    for (UBaseType_t i = 0; i < n; i++) {
        int core = xTaskGetCoreID(list[i].xHandle); // tskNO_AFFINITY = -1 ถ้าไม่ pin
        float cpu = total_runtime ?
            (100.0f * list[i].ulRunTimeCounter / total_runtime) : 0;

        printf("%-14s %-6u %-6d %-8lu %-5.1f\n",
               list[i].pcTaskName,
               list[i].uxCurrentPriority,
               core,
               list[i].usStackHighWaterMark,   // เหลือกี่ word ก่อน stack overflow
               cpu);
    }
    vPortFree(list);
}
int api_dumptask(lua_State *L) {
    debug_dump_tasks();
    return 0;
}
const gpio_num_t buttons[] = {GPIO_NUM_9,GPIO_NUM_10,GPIO_NUM_11};
int api_init_button(lua_State *L) {
    for (int i=0;i<3;i++) {
        gpio_set_direction(buttons[i],GPIO_MODE_INPUT);
        gpio_pulldown_en(buttons[i]);
        gpio_pullup_dis(buttons[i]);
    }
    return 0;
}
int api_get_button(lua_State *L) {
    lua_newtable(L);
    for (int i =0;i<3;i++) {
        lua_pushnumber(L,!gpio_get_level(buttons[i]));
        lua_rawseti(L,-2,i+1);
    }
    return 1;
}
static inline uint16_t rgb24_to_rgb565(uint32_t rgb)
{
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8)  & 0xFF;
    uint8_t b = rgb & 0xFF;

    return ((uint16_t)(r & 0xF8) << 8) |
           ((uint16_t)(g & 0xFC) << 3) |
           ((uint16_t)(b >> 3));
}
int api_rgb_convert(lua_State *L) {
    uint32_t rgb888 = (uint32_t)luaL_checkinteger(L,1);
    uint16_t rgb565 = rgb24_to_rgb565(rgb888);
    lua_pushinteger(L,rgb565);
    return 1;
}
void API_INIT(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L,api_get_button);
    lua_setfield(L,-2,"get");
    lua_pushcfunction(L,api_init_button);
    lua_setfield(L,-2,"init");
    lua_setglobal(L,"button");
    lua_pushcfunction(L,api_dumptask);
    lua_setglobal(L,"dumptask");
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_46,GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_NUM_46,GPIO_PULLUP_ENABLE));
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_NUM_46,GPIO_PULLDOWN_DISABLE));
    lua_newtable(L);
    lua_pushcfunction(L,api_getcursor);
    lua_setfield(L,-2,"pos");
    lua_pushcfunction(L,api_joybutton);
    lua_setfield(L,-2,"isDown");
    lua_setglobal(L,"cursor");
    lua_newtable(L);    
    lua_pushcfunction(L, api_time_millis);
    lua_setfield(L, -2, "millis");
    lua_setglobal(L, "time");
    lua_newtable(L);
    lua_pushcfunction(L, api_memory_free);
    lua_setfield(L, -2, "free");
    lua_pushcfunction(L, api_memory_used);
    lua_setfield(L, -2, "used");
    lua_setglobal(L, "memory");
    lua_pushcfunction(L,api_system_delay);
    lua_setglobal(L, "delay");
    lua_pushcfunction(L,api_read_file);
    lua_setglobal(L,"readfile");
    lua_newtable(L);
    lua_pushcfunction(L, api_gfx_line);     lua_setfield(L, -2, "line");
    lua_pushcfunction(L, api_gfx_rect);     lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, api_gfx_circle);   lua_setfield(L, -2, "circle");
    lua_pushcfunction(L, api_gfx_loadfont); lua_setfield(L, -2, "loadfont");
    lua_pushcfunction(L, api_gfx_text);     lua_setfield(L, -2, "text");
    lua_pushcfunction(L, api_gfx_push);     lua_setfield(L, -2, "push");
    lua_pushcfunction(L, api_rgb_convert);  lua_setfield(L, -2, "rgb");
    lua_setglobal(L, "gfx");
    lua_wifi_register(L);
    LOG_INFO("LOADED");
}