#ifndef CURSOR_POLL_H
#define CURSOR_POLL_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cursor.poll - wait for a button change; changes are recorded ALL THE TIME.
 *
 *   cursor.poll([ids [, timeout_ms]]) -> id, pressed, age_ms   (or nil on timeout)
 *   cursor.poll_flush([ids])                                    forget buffered changes
 *
 *   ids         nil            -> all 5 buttons
 *               integer 0..4   -> one button
 *               {0,2,3}        -> several
 *   timeout_ms  nil or < 0     -> wait forever
 *               0              -> do not wait: only return a change that is already buffered
 *               > 0            -> give up after that many ms (returns nil)
 *
 *   Button ids:   0 = primary   joystick push button  (GPIO46)
 *                 1 = secondary joystick push button  (GPIO14)
 *                 2 = KEY1                            (GPIO9)
 *                 3 = KEY2                            (GPIO10)
 *                 4 = KEY3                            (GPIO11)
 *
 *   pressed     true = pressed (pin low), false = released (pin high)
 *   age_ms      how long ago the change happened (the poller may have been busy)
 *
 * Semantics
 *   - A background task (started by cursor_poll_register) watches the 5 GPIOs from boot, debounces
 *     (CURSOR_POLL_DEBOUNCE_MS) and stores every settled change, whether or not Lua is polling.
 *   - cursor.poll returns the OLDEST buffered change of the requested buttons at once, or sleeps
 *     (queue, no busy loop) until one arrives. Changes of other buttons stay buffered.
 *   - The buffer holds 16 changes; when it is full the oldest is dropped.
 *   - Call cursor.poll_flush() when a screen/mode starts and old presses must not count.
 *   - Several buttons changing in the same debounce window are all kept (one per call).
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
