---@param proc Process
return function(proc)
    local ok, key = requestFC(proc)
    if not ok then error("cannot request full screen") end
    local isOwnedKey = true
    local total = memory.free()+memory.used()
    while proc.m.receiveSignal(proc) == proc.m.SIG_NON do
        if proc.signal == proc.m.SIG_TERM or proc.signal == proc.m.SIG_KILL then
            if isOwnedKey then giveFC(key); isOwnedKey = false end
            proc.status = proc.m.PROCESS_DEAD
            coroutine.yield()
            return
        end
        local bg = not proc:canDraw()
        if bg and isOwnedKey then
            giveFC(key)
            isOwnedKey = false
        elseif not bg and not isOwnedKey then
            ok, key = requestFC(proc)
            if ok then isOwnedKey = true end
        end
        if proc:canDraw() and isOwnedKey then
            -- code
            local SRAM = "SRAM [                                 ]"
            local startX = 8*6
            FILL(0)
            gfx.text(0,0,SRAM,0xFFFF)
            gfx.text(0,16,"Used "..tostring(memory.used()/1024).."KiB Free "..tostring(memory.free()/1024).."KiB",0xFFFF)
            local used = memory.used()
            local pencen = used/total
            gfx.rect(startX,2,math.floor(320*pencen),10,0xFFF,true)
            gfx.push()
            local btn = button.get()
            if btn[1] == 1 then
                proc.signal = proc.m.SIG_TERM
            end
            proc:delay(33.333)
        else
            -- bg
            proc:delay(16.667)
        end
    end
    if isOwnedKey then giveFC(key) end
end