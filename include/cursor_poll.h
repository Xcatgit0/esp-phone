#ifndef CURSOR_POLL_H
#define CURSOR_POLL_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cursor.poll - block the calling Lua task until a button changes state.
 *
 *   cursor.poll([ids [, timeout_ms]]) -> id, pressed   (or nil on timeout)
 *
 *   ids         nil            -> watch all 5 buttons
 *               integer 0..4   -> watch one button
 *               {0,2,3}        -> watch several
 *   timeout_ms  nil or < 0     -> wait forever
 *               0..            -> give up after that many ms (returns nil)
 *
 *   Button ids:   0 = primary   joystick push button  (GPIO46)
 *                 1 = secondary joystick push button  (GPIO14)
 *                 2 = KEY1                            (GPIO9)
 *                 3 = KEY2                            (GPIO10)
 *                 4 = KEY3                            (GPIO11)
 *
 *   pressed     true  = now pressed (pin low)
 *               false = now released (pin high)
 *
 * Semantics
 *   - Only changes that happen AFTER the call starts are reported. A button that is
 *     already held when poll() is called is the baseline; its *release* is the event.
 *     Nothing queued between two calls is ever delivered.
 *   - The task sleeps on a FreeRTOS queue fed by a GPIO interrupt: no busy loop.
 *   - Edges are debounced (CURSOR_POLL_DEBOUNCE_MS): a change is only reported if the
 *     level is still different from the baseline after the bounce window.
 *   - One event is returned per call. If two buttons change inside the same debounce
 *     window, the one that fired first wins; the other is "before the next poll" and is not kept.
 *   - Interrupts are armed only while poll() runs and removed before it returns.
 *   - Only one task may call poll() at a time (a second concurrent call raises a Lua error).
 */
#define CURSOR_POLL_DEBOUNCE_MS 10

/* Adds cursor.poll to the global `cursor` table (creates the table if missing).
 * Call once from API_INIT(), after the `cursor` table has been registered. */
void cursor_poll_register(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* CURSOR_POLL_H */
