local VectorEngine = require "VectorEngine"
local sprite       = require "sprite"
local at = {}
---@param proc Process
function at.Info(proc)
    local startY = 40
    local endY   = 200
    gfx.rect(0,startY,320,endY-startY,0,true)
    gfx.rect(0,startY,320,endY-startY,0xFFFF,false)
    VectorEngine.draw(0,startY,sprite["Info"],(endY-startY)/2,(endY-startY)/2)

    gfx.push()
    proc:delay(1000)
end
return at