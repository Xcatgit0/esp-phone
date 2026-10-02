-- Headless smoke test:  ./phone_emu --headless --run ../tests/smoke.lua --shot /tmp/smoke.bmp
local fails = 0
local function check(name, cond, extra)
  if cond then print("PASS  " .. name)
  else fails = fails + 1; print("FAIL  " .. name .. (extra and ("  (" .. tostring(extra) .. ")") or "")) end
end

-- 1. library set must match the device (no os/io/coroutine/utf8/package)
check("no io/os/package", io == nil and os == nil and package == nil and coroutine == nil)

-- 2. buttons (active low on GPIO 9/10/11)
button.init()
local b = button.get()
check("buttons released", b[1] == 0 and b[2] == 0 and b[3] == 0)
emu.key(2, true)
b = button.get()
check("KEY2 pressed", b[1] == 0 and b[2] == 1 and b[3] == 0)
emu.key(2, false)

-- 3. joystick buttons (cursor.raw returns x, y, level; 0 = pressed)
local x, y, lvl = cursor.raw(1)
check("joy1 centred", math.abs(x) < 0.05 and math.abs(y) < 0.05 and lvl == 1, x .. "," .. y)
emu.stick_button(1, true); delay(50)
local _, _, l2 = cursor.raw(1)
check("joy1 button pressed (0)", l2 == 0)
emu.stick_button(1, false)

-- 4. cursor movement: 1 px per 10 ms tick when stick is past the 2800/1200 thresholds
cursor.set(100, 100)
emu.stick(1, 1, 0)                         -- stick right -> cursor right
local t0 = time.millis(); delay(500)
emu.stick(1, 0, 0)
local cx, cy = cursor.pos()
check("cursor moved right ~50px in 500ms", cx > 130 and cx < 160 and cy == 100, "cx=" .. cx .. " (dt=" .. (time.millis() - t0) .. "ms)")
emu.stick(1, 0, 1); delay(300); emu.stick(1, 0, 0)
local _, cy2 = cursor.pos()
check("cursor moved down", cy2 > 120, cy2)

-- 5. secondary joystick axis mapping (device swaps raw axes)
emu.stick(2, 1, 0); delay(50)
local sx, sy = cursor.raw(2)
check("joy2 x+ -> sec_x ~ +1", sx > 0.9 and math.abs(sy) < 0.05, sx .. "," .. sy)
emu.stick(2, 0, 0)

-- 6. touch ADC: raw values derived from corner calibration in touch.lua
emu.touch(true, 160, 120); delay(30)
local rx, ry, z1, z2 = cursor.touch()
check("touch raw plausible (centre)", rx > 1500 and rx < 2500 and ry > 1500 and ry < 2500 and z1 > 60, rx .. "," .. ry .. "," .. z1 .. "," .. z2)
emu.touch(false); delay(30)
local _, _, z1b = cursor.touch()
check("touch released z1 < 60", z1b < 60, z1b)

-- 7. gfx -> LCD
gfx.rect(0, 0, 320, 240, gfx.rgb(0x000000), true)
gfx.rect(10, 10, 100, 50, gfx.rgb(0xFF0000), true)
gfx.circle(200, 120, 40, gfx.rgb(0x00FF00), true)
gfx.line(0, 239, 319, 0, gfx.rgb(0x0000FF))
cursor.visible(false)
gfx.push()
check("push reaches LCD", emu.wait_push(1, 1000))
check("red pixel", emu.pixel(20, 20) == 0xF800, emu.pixel(20, 20))
check("green pixel", emu.pixel(200, 120) == 0x07E0, emu.pixel(200, 120))

-- 8. tick quantisation: delay(5) -> pdMS_TO_TICKS(5) = 0 ticks
local a = time.millis(); for i = 1, 20 do delay(5) end
local dt = time.millis() - a
check("delay(5) is NOT 5ms on device (0 ticks)", dt < 50, dt .. "ms for 20x delay(5)")
a = time.millis(); delay(25); dt = time.millis() - a
check("delay(25) rounds down to 2 ticks (~20ms)", dt >= 19 and dt <= 31, dt)

-- 9. memory accounting
local f0 = memory.free()
local big = {}; for i = 1, 5000 do big[i] = "x" .. i end
check("memory.free decreases with allocations", memory.free() < f0 - 20000, f0 - memory.free())
big = nil; collectgarbage(); collectgarbage()
check("memory.free recovers after GC", memory.free() > f0 - 20000)

-- 10. wifi mock
local ok, err = wifi.mode("sta"); 
ok, err = wifi.start()
check("wifi.start ok", ok == true or ok == nil and err == nil, tostring(err))
emu.screenshot("/tmp/smoke_inline.bmp")

print(fails == 0 and "ALL PASSED" or (fails .. " FAILED"))
emu.quit()
