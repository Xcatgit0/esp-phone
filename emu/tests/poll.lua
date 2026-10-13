-- cursor.poll(): button ids 0=joy1 btn, 1=joy2 btn, 2=KEY1, 3=KEY2, 4=KEY3
-- Every change is recorded all the time; poll() returns the oldest buffered change (id, pressed, age_ms).
-- NOTE: emu.key(i) is 1-based (KEY1 = 1) whereas poll ids are 0-based (KEY1 = 2).
local fails = 0
local function check(name, cond, extra)
  if cond then print("PASS  " .. name)
  else fails = fails + 1; print("FAIL  " .. name .. (extra ~= nil and ("  (" .. tostring(extra) .. ")") or "")) end
end
local function timed(f, ...)
  local t0 = emu.cpu_ms(); local w0 = host.unix_ms()
  local a, b, c = f(...)
  return a, b, c, host.unix_ms() - w0, emu.cpu_ms() - t0
end
local function drain() local n = 0; while cursor.poll(nil, 0) do n = n + 1 end; return n end

check("cursor.poll / poll_flush exist", type(cursor.poll) == "function" and type(cursor.poll_flush) == "function")
check("buffer empty at start", drain() == 0)

-- 1. timeout
local id, pressed, age, wall = timed(cursor.poll, 2, 120)
check("timeout returns nil after ~120ms", id == nil and wall >= 110 and wall <= 200, tostring(id) .. " " .. wall .. "ms")
check("timeout 0 returns at once", select(4, timed(cursor.poll, 2, 0)) < 20)

-- 2. press while blocked in poll; sleeps instead of spinning
emu.schedule(100, "key", 1, true)
local id, pressed, age, wall, cpu = timed(cursor.poll, 2, 1000)
check("KEY1 press -> 2,true", id == 2 and pressed == true, tostring(id) .. "," .. tostring(pressed))
check("woke at ~100ms (+debounce)", wall >= 95 and wall <= 250, wall)
check("no busy loop (cpu << wall)", cpu < wall * 0.3, string.format("cpu %.1f ms of %d ms", cpu, wall))
check("age ~ debounce window", age >= 0 and age <= 80, age)

-- 3. release
emu.schedule(80, "key", 1, false)
id, pressed = cursor.poll(2, 1000)
check("KEY1 release -> 2,false", id == 2 and pressed == false)

-- 4. THE POINT: changes while nobody polls are kept, in order, with their age
emu.key(1, true); delay(60); emu.key(1, false); delay(60); emu.key(1, true); delay(60)
local a1, p1, g1 = cursor.poll(2, 0)
local a2, p2, g2 = cursor.poll(2, 0)
local a3, p3, g3 = cursor.poll(2, 0)
local a4 = cursor.poll(2, 0)
check("3 buffered changes come back in order", a1 == 2 and p1 == true and a2 == 2 and p2 == false and a3 == 2 and p3 == true and a4 == nil,
      tostring(p1) .. tostring(p2) .. tostring(p3))
check("age reports how stale they are", g1 >= 150 and g1 <= 400 and g2 >= 90 and g2 < g1 and g3 < g2 and g3 >= 30, g1 .. "," .. g2 .. "," .. g3)
emu.key(1, false); delay(40); drain()

-- 5. no change -> nothing, even though the button is held
emu.key(3, true); delay(40)
id, pressed = cursor.poll(4, 0)
check("held press was recorded once", id == 4 and pressed == true)
id = cursor.poll(4, 150)
check("holding does not generate more events", id == nil, id)
emu.schedule(50, "key", 3, false)
id, pressed = cursor.poll(4, 1000)
check("release -> 4,false", id == 4 and pressed == false)

-- 6. flush
emu.key(2, true); emu.key(2, false); delay(60)
cursor.poll_flush()
check("poll_flush() empties the buffer", cursor.poll(nil, 0) == nil)
emu.key(2, true); delay(40); emu.key(3, true); delay(40)
cursor.poll_flush(3)                                        -- poll id 3 = KEY2 (emu.key(2))
check("poll_flush(3) removed KEY2's change", cursor.poll(3, 0) == nil)
id, pressed = cursor.poll(nil, 0)
check("...and kept KEY3's", id == 4 and pressed == true, id)
drain(); emu.key(2, false); emu.key(3, false); delay(40); drain()

-- 7. changes of unwatched buttons stay buffered for whoever polls them
emu.key(1, true); delay(40); emu.key(3, true); delay(40)
id, pressed = cursor.poll({ 4 }, 100)
check("poll({4}) skips the older KEY1 change", id == 4 and pressed == true, id)
id, pressed = cursor.poll(2, 0)
check("...and KEY1's change is still there", id == 2 and pressed == true, id)
emu.key(1, false); emu.key(3, false); delay(40); drain()

-- 8. nil ids = any; joystick buttons
emu.schedule(60, "stick_button", 2, true)
id, pressed = cursor.poll(nil, 1000)
check("any: joy2 button -> 1,true", id == 1 and pressed == true, tostring(id))
emu.stick_button(2, false); delay(40); drain()
emu.stick_button(1, true); delay(40)
id, pressed = cursor.poll(0, 0)
check("joy1 button -> 0,true", id == 0 and pressed == true)
emu.stick_button(1, false); delay(40); drain()

-- 9. two buttons changing inside one debounce window: both are kept
emu.key(1, true); emu.key(2, true); delay(60)
local x1, _, _ = cursor.poll(nil, 0)
local x2, _, _ = cursor.poll(nil, 0)
check("simultaneous presses -> two events", x1 ~= nil and x2 ~= nil and x1 ~= x2 and (x1 + x2) == 5, tostring(x1) .. "," .. tostring(x2))
emu.key(1, false); emu.key(2, false); delay(60); drain()

-- 10. bounce is filtered: down-up-down within 3 ms => ONE press
emu.schedule(100, "key", 2, true); emu.schedule(101, "key", 2, false); emu.schedule(102, "key", 2, true)
id, pressed = cursor.poll(3, 1000)
check("bounce -> single press 3,true", id == 3 and pressed == true, tostring(id) .. "," .. tostring(pressed))
id = cursor.poll(3, 200)
check("no extra events after a bounce", id == nil, id)
emu.key(2, false); delay(40); drain()

-- 11. glitch shorter than the debounce window is ignored
emu.schedule(100, "key", 2, true); emu.schedule(103, "key", 2, false)
id = cursor.poll(3, 300)
check("3 ms glitch ignored", id == nil, id)

-- 12. buffer holds 16 changes; the oldest are dropped
for i = 1, 20 do emu.key(1, i % 2 == 1); delay(30) end        -- 20 changes: press,release,press,...
local n, first_pressed = 0, nil
while true do
  local a, p = cursor.poll(2, 0)
  if not a then break end
  n = n + 1; if n == 1 then first_pressed = p end
end
check("only the newest 16 of 20 changes are kept", n == 16, n)
check("...the oldest 4 were dropped (5th change is a press)", first_pressed == true)

-- 13. argument validation
check("id 7 rejected", not pcall(cursor.poll, 7, 10))
check("empty list rejected", not pcall(cursor.poll, {}, 10))
check("list with bad id rejected", not pcall(cursor.poll, {1, 9}, 10))
check("string id rejected", not pcall(cursor.poll, "x", 10))
check("poll_flush(9) rejected", not pcall(cursor.poll_flush, 9))

-- 14. raw GPIO edges are recorded too
emu.gpio(9, 0); delay(40)
id, pressed = cursor.poll(2, 0)
check("emu.gpio level change -> event", id == 2 and pressed == true)
emu.gpio(9, 1); delay(40); drain()

print(fails == 0 and "ALL PASSED" or (fails .. " FAILED"))
emu.quit()
