function FILL(r,filled) 
    gfx.rect(0,0,320,240,r,filled or true)
end

button.init()
gfx.loadfont("/fs/font.psf")
local W = 320
local H = 240
_G.loaded = {}
function _G.require(name)
    if _G.loaded[name] then return _G.loaded[name] end
    local code = readfile("/fs/"..name..".lua")
    if not code then return nil end
    local func = load(code)
    if not func then return nil end
    local lib = func()
    _G.loaded[name] = lib
    return lib
end
loadfile("/fs/keyboard.lua")()
local touch = require("touch")
touch.recalibrate()
local colors = {
    0xF800, -- Red
    0x07E0, -- Green
    0x001F, -- Blue
    0xFFE0, -- Yellow
    0x07FF, -- Cyan
    0xF81F, -- Magenta
    0xFFFF, -- White
    0x0000, -- Black
}

local barH = math.floor(H / #colors)

for i, color in ipairs(colors) do
    local y = (i - 1) * barH

    gfx.rect(
        0,
        y,
        W,
        barH,
        color,
        true
    )
end

gfx.push()
delay(250)
FILL(0)
gfx.push()
local process = require("process")
process.create("touch","/fs/touch_proc.lua",true)
process.create("Desktop","/fs/desktop.lua")
process.scheduler()