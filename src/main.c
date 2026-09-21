#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "tft.h"
#include "cursor.h"
#include "log.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "wrap.h"
#define EEC ESP_ERROR_CHECK
void list_spiffs(void)
{
    DIR *dir = opendir("/fs");
    if (!dir)
    {
        printf("เปิด /fs ไม่ได้\n");
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        printf("%s\n", entry->d_name);
    }

    closedir(dir);
}

lua_State *L;
extern void initAPI(lua_State *L);
static void lua_task(void *arg)
{
    //printf("ram %u\n",heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    lua_State *L = arg; 
        // รัน Lua
        int status = lua_pcall(L, 0, 0, 0);

        if (status != LUA_OK)
        {
            printf("Lua error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
        vTaskDelay(1);
        LOG_WARN("LUA VM STOPPED");
        LOG_WARN("RESET TO RESTART");

        while (1) vTaskDelay(1000);
}
void app_main(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/fs",
        .partition_label = "storage",
        .max_files = 16,
        .format_if_mount_failed = true,
    };

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    size_t total = 0;
    size_t used = 0;

    ESP_ERROR_CHECK(
        esp_spiffs_info("storage", &total, &used));
    LOG_INFO("SPIFFS: %zu / %zu bytes", used, total);
    list_spiffs();
    LOG_INFO("Initializing tft");
    tft_init();
    LOG_INFO("Initializing Joystick and Cursor");
    joystick_init();
    LOG_INFO("DONE");
    LOG_INFO("Loading Lua");
    printf("ram %u\n",heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    L = luaL_newstate();
    //lua_sethook(L, lua_debug_hook, LUA_MASKCOUNT, 10);
    LOG_INFO("Lua loaded");
    LOG_INFO("Loading API..");
    initAPI(L);
    char *bootstrap = read_file("/fs/bootstrap.lua");
    if (bootstrap == NULL)
    {
        LOG_ERROR("CANNOT LOAD \"bootstrap.lua\"");
        abort();
    }
    (void)luaL_loadbuffer(L, bootstrap, strlen(bootstrap), "bootstrap.lua");
    free(bootstrap);

    xTaskCreatePinnedToCore(
        lua_task,
        "lua",
        8192,
        L,
        5,
        NULL,
        1 // Core 1
    );
    vTaskDelay(pdMS_TO_TICKS(2000));
    //heap_caps_dump_all();
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void initAPI(lua_State *L)
{
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);

    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);

    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);

    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    LOG_INFO("Loading api");
    API_INIT(L);
}