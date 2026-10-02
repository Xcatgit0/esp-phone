/*
 * cursor_poll.c - cursor.poll(): sleep until a button changes (interrupt driven).
 * See include/cursor_poll.h for the Lua-level contract.
 */
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
#define POLL_QUEUE_LEN 8
#define POLL_PRESSED_LEVEL 0 /* active low */

static QueueHandle_t poll_queue = NULL;
static bool poll_isr_service_ready = false;
static volatile bool poll_busy = false;

/* GPIO ISR: only tells the waiting task which button saw an edge. */
static void IRAM_ATTR poll_isr(void *arg)
{
    uint8_t id = (uint8_t)(uintptr_t)arg;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(poll_queue, &id, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

/* Wait for a settled change on one of the buttons in `mask`.
 * On return: *out_id = -1 on timeout, otherwise the button id and its settled state. */
static esp_err_t poll_wait(uint32_t mask, int64_t timeout_ms, int *out_id, int *out_pressed)
{
    int baseline[POLL_NUM_BUTTONS] = {0};
    uint32_t armed = 0;
    esp_err_t err = ESP_OK;
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000;
    const int64_t deadline_us = (timeout_ms < 0) ? 0 : esp_timer_get_time() + timeout_ms * 1000;

    *out_id = -1;
    *out_pressed = 0;

    /* 1) arm interrupts */
    for (int i = 0; i < POLL_NUM_BUTTONS; i++)
    {
        if (!(mask & (1u << i)))
            continue;
        gpio_set_direction(poll_gpio[i], GPIO_MODE_INPUT); /* make sure the input buffer is on; pulls untouched */
        err = gpio_set_intr_type(poll_gpio[i], GPIO_INTR_ANYEDGE);
        if (err != ESP_OK)
            goto done;
        err = gpio_isr_handler_add(poll_gpio[i], poll_isr, (void *)(uintptr_t)i);
        if (err != ESP_OK)
            goto done;
        armed |= (1u << i);
    }

    /* 2) drop anything that fired while arming, THEN take the baseline.
     *    (reset first: an edge between reset and the read is harmless, it just
     *     re-reads as "no change"; the opposite order could lose a real change) */
    xQueueReset(poll_queue);
    for (int i = 0; i < POLL_NUM_BUTTONS; i++)
        if (armed & (1u << i))
            baseline[i] = gpio_get_level(poll_gpio[i]);

    /* 3) sleep until an edge, let the contacts settle, compare with the baseline */
    for (;;)
    {
        TickType_t wait = portMAX_DELAY;
        if (timeout_ms >= 0)
        {
            int64_t remaining_us = deadline_us - esp_timer_get_time();
            if (remaining_us <= 0)
                break;
            int64_t ticks = (remaining_us + tick_us - 1) / tick_us; /* round up */
            wait = (ticks >= (int64_t)portMAX_DELAY) ? (portMAX_DELAY - 1) : (TickType_t)ticks;
        }

        uint8_t ev;
        if (xQueueReceive(poll_queue, &ev, wait) != pdPASS)
            break; /* timeout */

        vTaskDelay(pdMS_TO_TICKS(CURSOR_POLL_DEBOUNCE_MS)); /* bounce window */
        xQueueReset(poll_queue);                            /* edges produced by the bounce */

        int pick = -1, level = 0;
        if (ev < POLL_NUM_BUTTONS && (armed & (1u << ev)))
        {
            level = gpio_get_level(poll_gpio[ev]);
            if (level != baseline[ev])
                pick = ev;
        }
        if (pick < 0)
        {
            for (int i = 0; i < POLL_NUM_BUTTONS; i++)
            {
                if (!(armed & (1u << i)))
                    continue;
                level = gpio_get_level(poll_gpio[i]);
                if (level != baseline[i])
                {
                    pick = i;
                    break;
                }
            }
        }
        if (pick >= 0)
        {
            *out_id = pick;
            *out_pressed = (level == POLL_PRESSED_LEVEL);
            break;
        }
        /* it was only bounce / a glitch: keep waiting with the remaining timeout */
    }

done:
    /* 4) disarm: nothing may fire (or queue up) once poll() has returned */
    for (int i = 0; i < POLL_NUM_BUTTONS; i++)
    {
        if (!(armed & (1u << i)))
            continue;
        gpio_isr_handler_remove(poll_gpio[i]);
        gpio_set_intr_type(poll_gpio[i], GPIO_INTR_DISABLE);
    }
    return err;
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

    if (poll_queue == NULL)
    {
        poll_queue = xQueueCreate(POLL_QUEUE_LEN, sizeof(uint8_t));
        if (poll_queue == NULL)
            return luaL_error(L, "cursor.poll: out of memory");
    }
    if (!poll_isr_service_ready)
    {
        esp_err_t e = gpio_install_isr_service(0);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) /* INVALID_STATE = somebody installed it already */
            return luaL_error(L, "cursor.poll: gpio isr service: %s", esp_err_to_name(e));
        poll_isr_service_ready = true;
    }

    /* no Lua API that can raise an error between arming and disarming */
    int id = -1, pressed = 0;
    poll_busy = true;
    esp_err_t err = poll_wait(mask, timeout_ms, &id, &pressed);
    poll_busy = false;

    if (err != ESP_OK)
        return luaL_error(L, "cursor.poll: %s", esp_err_to_name(err));

    if (id < 0)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, id);
    lua_pushboolean(L, pressed);
    return 2;
}

void cursor_poll_register(lua_State *L)
{
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
    lua_pop(L, 1);
}
