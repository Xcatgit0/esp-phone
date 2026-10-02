-- How long does gfx.push() take to reach the LCD when the system is idle vs. busy?
local function measure(label, gap_ms, n)
  if n == 0 then return end
  local worst, sum = 0, 0
  for i = 1, n do
    emu.sleep(gap_ms + math.random(0, 300))
    gfx.rect(0, 0, 10, 10, i % 2 == 0 and 0xFFFF or 0x0000, true)
    local before = emu.pushes()
    local t0 = host.unix_ms()
    gfx.push()
    while emu.pushes() == before and host.unix_ms() - t0 < 2000 do emu.sleep(1) end
    local dt = host.unix_ms() - t0
    sum = sum + dt; if dt > worst then worst = dt end
  end
  print(string.format("%-34s avg %4d ms  worst %4d ms", label, sum // n, worst))
end
math.randomseed(host.unix_ms())
measure("sporadic (random 100-400 ms gaps)", 100, 40)
measure("busy (pushes every ~20 ms)", 0, 0)
local t0 = host.unix_ms(); local n0 = emu.pushes()
for i = 1, 60 do gfx.rect(0,0,10,10,i,true); gfx.push(); delay(20) end
local el = host.unix_ms() - t0
print(string.format("%-34s %d of 60 pushes reached the LCD in %d ms", "burst (push every 20 ms)", emu.pushes() - n0, el))
emu.quit()
