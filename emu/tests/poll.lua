-- cursor.poll(): button ids 0=joy1 btn, 1=joy2 btn, 2=KEY1, 3=KEY2, 4=KEY3
-- NOTE: emu.key(i) is 1-based (KEY1 = 1) whereas poll ids are 0-based (KEY1 = 2).
local fails = 0
local function check(name, cond, extra)
  if cond then print("PASS  " .. name)
  else fails = fails + 1; print("FAIL  " .. name .. (extra ~= nil and ("  (" .. tostring(extra) .. ")") or "")) end
end
local function timed(f, ...)
  local t0 = emu.cpu_ms(); local w0 = host.unix_ms()
  local a, b = f(...)
  return a, b, host.unix_ms() - w0, emu.cpu_ms() - t0
end

check("cursor.poll exists", type(cursor.poll) == "function")

-- 1. timeout
local id, pressed, wall = timed(cursor.poll, 2, 120)
check("timeout returns nil after ~120ms", id == nil and pressed == nil and wall >= 110 and wall <= 200, tostring(id) .. " " .. wall .. "ms")

-- 2. press after the call starts; and it sleeps instead of spinning
emu.schedule(100, "key", 1, true)
local id, pressed, wall, cpu = timed(cursor.poll, 2, 1000)
check("KEY1 press -> 2,true", id == 2 and pressed == true, tostring(id) .. "," .. tostring(pressed))
check("woke at ~100ms (+debounce)", wall >= 95 and wall <= 250, wall)
check("no busy loop (cpu << wall)", cpu < wall * 0.3, string.format("cpu %.1f ms of %d ms", cpu, wall))

-- 3. held at call time = baseline; release is the event
emu.schedule(80, "key", 1, false)
id, pressed = cursor.poll(2, 1000)
check("KEY1 release -> 2,false", id == 2 and pressed == false, tostring(id) .. "," .. tostring(pressed))

-- 4. stale data ignored: press+hold happened BEFORE poll
emu.key(3, true); delay(50)
id = cursor.poll(4, 150)
check("press before poll is not reported", id == nil, id)
emu.schedule(50, "key", 3, false)
id, pressed = cursor.poll(4, 1000)
check("release of held KEY3 -> 4,false", id == 4 and pressed == false)

-- 5. a press+release that both happen while NOT polling leaves nothing queued
emu.key(2, true); emu.key(2, false); delay(30)
id = cursor.poll(3, 120)
check("events between polls are dropped", id == nil, id)

-- 6. nil ids = any button; joystick buttons
emu.schedule(60, "stick_button", 2, true)
id, pressed = cursor.poll(nil, 1000)
check("any: joy2 button -> 1,true", id == 1 and pressed == true, tostring(id))
emu.schedule(40, "stick_button", 2, false)
cursor.poll(1, 1000)
emu.schedule(60, "stick_button", 1, true)
id, pressed = cursor.poll(0, 1000)
check("joy1 button -> 0,true", id == 0 and pressed == true)
emu.stick_button(1, false)

-- 7. list of ids, other buttons ignored
emu.schedule(40, "key", 1, true)      -- KEY1 (id 2): not watched
emu.schedule(120, "key", 3, true)     -- KEY3 (id 4): watched
id, pressed = cursor.poll({3, 4}, 1000)
check("list {3,4}: ignores KEY1, reports KEY3", id == 4 and pressed == true, tostring(id))
emu.key(1, false); emu.key(3, false); delay(30)

-- 8. contact bounce is filtered: down-up-down within 3 ms => ONE press
emu.schedule(100, "key", 2, true); emu.schedule(101, "key", 2, false); emu.schedule(102, "key", 2, true)
id, pressed = cursor.poll(3, 1000)
check("bounce -> single press 3,true", id == 3 and pressed == true, tostring(id) .. "," .. tostring(pressed))
id = cursor.poll(3, 200)
check("no extra events after a bounce", id == nil, id)
emu.key(2, false); delay(30)

-- 9. glitch shorter than the debounce window is ignored
emu.schedule(100, "key", 2, true); emu.schedule(103, "key", 2, false)
id = cursor.poll(3, 300)
check("3 ms glitch ignored", id == nil, id)

-- 10. argument validation
check("id 7 rejected", not pcall(cursor.poll, 7, 10))
check("empty list rejected", not pcall(cursor.poll, {}, 10))
check("list with bad id rejected", not pcall(cursor.poll, {1, 9}, 10))
check("string id rejected", not pcall(cursor.poll, "x", 10))

-- 11. interrupts are disarmed after poll returns: raw GPIO edges leave no residue
emu.gpio(9, 0); emu.gpio(9, 1); delay(30)
id = cursor.poll(2, 100)
check("no residue from edges after poll", id == nil, id)

print(fails == 0 and "ALL PASSED" or (fails .. " FAILED"))
emu.quit()
