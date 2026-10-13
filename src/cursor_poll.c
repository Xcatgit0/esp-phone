/*
 * cursor_poll.c - cursor.poll(): sleep until a button changes (interrupt driven).
 * See include/cursor_poll.h for the Lua-level contract.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "lua.h"
#include "lauxlib.h"
#include "cursor_poll.h"

/* button id -> GPIO.  Keep in sync with api.c (`buttons[]`, GPIO46 / GPIO14 in api_joybutton). */
#define POLL_NUM_BUTTONS 5
static const gpio_num_t poll_gpio[POLL_NUM_BUTTONS] = {
    GPIO_NUM_46, /* 0 primary   joystick button */
    GPIO_NUM_14, /* 1 secondary joystick button */
    GPIO_NUM_9,  /* 2 KEY1 */
    GPIO_NUM_10, /* 3 KEY2 */
    GPIO_NUM_11, /* 4 KEY3 */
};
#define POLL_ALL_MASK ((1u << POLL_NUM_BUTTONS) - 1u)
#define POLL_EDGE_QUEUE_LEN 16
#define POLL_EVENT_CAP 16 /* buffered changes; when full the OLDEST is dropped */
#define POLL_TASK_STACK 3072
#define POLL_TASK_PRIO 6 /* above the Lua task (5): changes get timestamped promptly */
#define POLL_TASK_CORE 0
#define POLL_PRESSED_LEVEL 0 /* active low */

typedef struct
{
    int64_t t_us; /* esp_timer time of the first edge of this change */
    uint8_t id;
    uint8_t pressed;
} poll_event_t;

static poll_event_t s_ev[POLL_EVENT_CAP]; /* oldest first */
static int s_ev_n = 0;
static SemaphoreHandle_t s_ev_lock = NULL;
static QueueHandle_t s_edge_q = NULL; /* ISR -> button task: raw edges (button id) */
static QueueHandle_t s_wake_q = NULL; /* button task -> Lua: "a change was buffered" */
static int s_last[POLL_NUM_BUTTONS];  /* last settled level of every button */
static bool s_armed = false;
static volatile bool poll_busy = false;

/* GPIO ISR: only tells the button task which button saw an edge. */
static void IRAM_ATTR poll_isr(void *arg)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_edge_q, &id, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

static void poll_push_event(int id, bool pressed, int64_t t_us)
{
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    if (s_ev_n == POLL_EVENT_CAP) /* nobody is reading: forget the oldest change */
    {
        memmove(&s_ev[0], &s_ev[1], (POLL_EVENT_CAP - 1) * sizeof(s_ev[0]));
        s_ev_n--;
    }
    s_ev[s_ev_n].t_us = t_us;
    s_ev[s_ev_n].id = (uint8_t)id;
    s_ev[s_ev_n].pressed = pressed ? 1 : 0;
    s_ev_n++;
    xSemaphoreGive(s_ev_lock);

    uint8_t w = 1;
    xQueueOverwrite(s_wake_q, &w);
}

/* Runs all the time, independent of what the Lua task is doing: debounces edges and
 * records every settled change in the event buffer. */
static void poll_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        uint8_t id;
        if (xQueueReceive(s_edge_q, &id, portMAX_DELAY) != pdPASS)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const int64_t t_edge = esp_timer_get_time();
        uint32_t dirty = 1u << id;

        vTaskDelay(pdMS_TO_TICKS(CURSOR_POLL_DEBOUNCE_MS)); /* bounce window */
        while (xQueueReceive(s_edge_q, &id, 0) == pdPASS)   /* bounce edges + other buttons */
            dirty |= 1u << id;

        for (int i = 0; i < POLL_NUM_BUTTONS; i++)
        {
            if (!(dirty & (1u << i)))
                continue;
            int level = gpio_get_level(poll_gpio[i]);
            if (level == s_last[i])
                continue; /* bounce / glitch: back where it was */
            s_last[i] = level;
            poll_push_event(i, level == POLL_PRESSED_LEVEL, t_edge);
        }
    }
}

/* Start watching all buttons (once). Called from cursor_poll_register(); poll() retries if it failed. */
static esp_err_t poll_arm(void)
{
    if (s_armed)
        return ESP_OK;

    if (!s_ev_lock)
        s_ev_lock = xSemaphoreCreateMutex();
    if (!s_edge_q)
        s_edge_q = xQueueCreate(POLL_EDGE_QUEUE_LEN, sizeof(uint8_t));
    if (!s_wake_q)
        s_wake_q = xQueueCreate(1, sizeof(uint8_t));
    if (!s_ev_lock || !s_edge_q || !s_wake_q)
        return ESP_ERR_NO_MEM;

    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) /* INVALID_STATE = somebody installed it already */
        return e;

    for (int i = 0; i < POLL_NUM_BUTTONS; i++)
    {
        gpio_set_direction(poll_gpio[i], GPIO_MODE_INPUT); /* input buffer on; pulls untouched */
        if ((e = gpio_set_intr_type(poll_gpio[i], GPIO_INTR_ANYEDGE)) != ESP_OK)
            return e;
        if ((e = gpio_isr_handler_add(poll_gpio[i], poll_isr, (void *)(uintptr_t)i)) != ESP_OK)
            return e;
    }
    for (int i = 0; i < POLL_NUM_BUTTONS; i++) /* after the ISRs are live: an edge in between is just ignored */
        s_last[i] = gpio_get_level(poll_gpio[i]);

    if (xTaskCreatePinnedToCore(poll_task, "btnpoll", POLL_TASK_STACK, NULL, POLL_TASK_PRIO, NULL,
                                POLL_TASK_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;

    s_armed = true;
    return ESP_OK;
}

/* Oldest buffered change whose button is in `mask` (removed from the buffer). Changes of other
 * buttons stay buffered for whoever polls them. */
static bool poll_take(uint32_t mask, poll_event_t *out)
{
    bool found = false;
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    for (int i = 0; i < s_ev_n; i++)
    {
        if (!(mask & (1u << s_ev[i].id)))
            continue;
        *out = s_ev[i];
        memmove(&s_ev[i], &s_ev[i + 1], (size_t)(s_ev_n - i - 1) * sizeof(s_ev[0]));
        s_ev_n--;
        found = true;
        break;
    }
    xSemaphoreGive(s_ev_lock);
    return found;
}

static void poll_flush(uint32_t mask)
{
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    int keep = 0;
    for (int i = 0; i < s_ev_n; i++)
        if (!(mask & (1u << s_ev[i].id)))
            s_ev[keep++] = s_ev[i];
    s_ev_n = keep;
    xSemaphoreGive(s_ev_lock);
}
/* ids argument: nil -> all, integer -> one, table -> list */
static uint32_t poll_parse_mask(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx))
        return POLL_ALL_MASK;

    if (lua_istable(L, idx))
    {
        uint32_t mask = 0;
        lua_Unsigned n = lua_rawlen(L, idx);
        luaL_argcheck(L, n > 0, idx, "empty button list");
        for (lua_Unsigned k = 1; k <= n; k++)
        {
            int isnum = 0;
            lua_rawgeti(L, idx, (lua_Integer)k);
            lua_Integer id = lua_tointegerx(L, -1, &isnum);
            lua_pop(L, 1);
            luaL_argcheck(L, isnum && id >= 0 && id < POLL_NUM_BUTTONS, idx, "button id must be 0..4");
            mask |= 1u << id;
        }
        return mask;
    }

    lua_Integer id = luaL_checkinteger(L, idx);
    luaL_argcheck(L, id >= 0 && id < POLL_NUM_BUTTONS, idx, "button id must be 0..4");
    return 1u << id;
}
static int api_cursor_poll(lua_State *L)
{
    uint32_t mask = poll_parse_mask(L, 1);

    int64_t timeout_ms = -1; /* forever */
    if (!lua_isnoneornil(L, 2))
    {
        lua_Number t = luaL_checknumber(L, 2);
        if (t >= 0)
            timeout_ms = (t > 2147483647.0) ? 2147483647 : (int64_t)t;
    }

    if (poll_busy)
        return luaL_error(L, "cursor.poll: another poll is already waiting");

    esp_err_t err = poll_arm();
    if (err != ESP_OK)
        return luaL_error(L, "cursor.poll: %s", esp_err_to_name(err));

    /* no Lua API that can raise an error while we wait */
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000;
    const int64_t deadline_us = (timeout_ms < 0) ? 0 : esp_timer_get_time() + timeout_ms * 1000;
    poll_event_t ev;
    bool got = false;

    poll_busy = true;
    for (;;)
    {
        if (poll_take(mask, &ev)) /* a change that already happened is returned at once */
        {
            got = true;
            break;
        }
        TickType_t wait = portMAX_DELAY;
        if (timeout_ms >= 0)
        {
            int64_t remaining_us = deadline_us - esp_timer_get_time();
            if (remaining_us <= 0)
                break;
            int64_t ticks = (remaining_us + tick_us - 1) / tick_us; /* round up */
            wait = (ticks >= (int64_t)portMAX_DELAY) ? (portMAX_DELAY - 1) : (TickType_t)ticks;
        }
        uint8_t w;
        if (xQueueReceive(s_wake_q, &w, wait) != pdPASS && timeout_ms < 0)
            break; /* never happens on the device (portMAX_DELAY); lets the emulator interrupt a reset */
    }
    poll_busy = false;

    if (!got)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, ev.id);
    lua_pushboolean(L, ev.pressed);
    lua_pushinteger(L, (lua_Integer)((esp_timer_get_time() - ev.t_us) / 1000)); /* how long ago it happened, ms */
    return 3;
}

/* cursor.poll_flush([ids]): forget buffered changes (all buttons, one id, or a list) */
static int api_cursor_poll_flush(lua_State *L)
{
    uint32_t mask = poll_parse_mask(L, 1);
    esp_err_t err = poll_arm();
    if (err != ESP_OK)
        return luaL_error(L, "cursor.poll_flush: %s", esp_err_to_name(err));
    poll_flush(mask);
    return 0;
}

void cursor_poll_register(lua_State *L)
{
    if (poll_arm() == ESP_OK)
        poll_flush(POLL_ALL_MASK); /* new VM (emulator reset): start with an empty buffer */

    lua_getglobal(L, "cursor");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "cursor");
    }
    lua_pushcfunction(L, api_cursor_poll);
    lua_setfield(L, -2, "poll");
    lua_pushcfunction(L, api_cursor_poll_flush);
    lua_setfield(L, -2, "poll_flush");
    lua_pop(L, 1);
}