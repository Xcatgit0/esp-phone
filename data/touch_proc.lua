local touch = require("touch")
---@param proc Process
return function(proc)
    local frame = 0
    while true do
        local x,y = touch.read()
        if x and y then
            touch.touching = true
            touch.x = x
            touch.y = y
            frame = frame + 1
            touch.down = touch.touching and (not (frame>1))
            touch.holding = (frame>4)
        else
            frame = 0
            touch.touching = false
            touch.clicked = false
            touch.holding = false
        end
        --print(memory.used()/1024)
        proc:delay(16.667)
    end
end