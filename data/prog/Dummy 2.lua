---@param proc Process
return function(proc)
    local ok,key = requestFC(proc)
    local isOwnedKey = true
    if not ok then error("cannot request full screen") end
    local bg = false
    while (proc.m.receiveSignal(proc)==proc.m.SIG_NON) do
        if proc.signal == proc.m.SIG_TERM or proc.signal == proc.m.SIG_KILL then
            proc.status = proc.m.PROCESS_DEAD
            coroutine.yield()
        end
        bg = not proc:canDraw()
        if bg then giveFC(key); isOwnedKey = false end
        if not isOwnedKey then ok,key = requestFC(proc); isOwnedKey = true end
        if proc:canDraw() then 
            -- code
            local chan, st = proc:poll({0,1,2,3,4})
            print(chan,st,isOwnedKey)
            if chan == 1 then
            print("bg",isOwnedKey)
            proc.runState  ="background" end
        else
            -- bg
            proc:delay(16.667)
        end
    end
    giveFC(key)
end