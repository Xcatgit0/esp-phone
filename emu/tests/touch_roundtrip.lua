-- Does data/touch.lua map the emulated raw ADC values back to the clicked screen position?
touch = dofile("/fs/touch.lua")
touch.recalibrate()
local worst = 0
for _, p in ipairs({{10,10},{160,120},{310,10},{10,230},{310,230},{80,60},{240,180}}) do
  emu.touch(true, p[1], p[2]); delay(50)
  local x, y = touch.read()
  if x then
    local e = math.max(math.abs(x - p[1]), math.abs(y - p[2]))
    if e > worst then worst = e end
    print(string.format("screen (%3d,%3d) -> touch.read (%s,%s)  err %d px", p[1], p[2], x, y, e))
  else
    print(string.format("screen (%3d,%3d) -> touch.read returned nil (unstable/not pressed)", p[1], p[2]))
  end
end
emu.touch(false); delay(50)
print("released ->", touch.read())
print("worst error px:", worst)
emu.quit()
