---@param proc Process
return function(proc)
    local ok, key = requestFC(proc)
    if not ok then error("cannot request full screen") end
    local isOwnedKey = true
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
            
        else
            -- bg
            proc:delay(16.667)
        end
    end
    if isOwnedKey then giveFC(key) end
end