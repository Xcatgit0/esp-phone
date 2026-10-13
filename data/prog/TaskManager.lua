---@param proc Process
return function(proc)
    local ok,key = requestFC(proc)
    local isOwnedKey = true
    if not ok then error("cannot request full screen") end
    local bg = false
    local lastbutton = {0,0,0}
    local function drawProcess() 
        FILL(0)
        gfx.text(5,0,"Click Process to kill or terminate",gfx.rgb(0x8888ff))
        for i=1,#proc.m.procs do
            local cx,cy = cursor.pos()
            local ismouseon = math.floor(cy/16) == i
            local ismon = (ismouseon and 3 ) or 0

            gfx.text(5+ismon,(16*(i))+ismon,tostring(i).."# "..proc.m.procs[i].name,(ismouseon and gfx.rgb(0xf1f1f1) or gfx.rgb(0x5555FF)))
                        if ismouseon then
                gfx.line(5,(16*(i)),5+ismon,(16*(i))+ismon,0xFFFF)
                gfx.line(5,(16*(i)),5+(8*(2+#proc.m.procs[i].name)),(16*(i)),0xFFFF)
                gfx.line(5+(8*(2+#proc.m.procs[i].name)),(16*(i)),5+(8*(2+#proc.m.procs[i].name))+5+ismon,(16*(i))+ismon,0xFFFF)
                gfx.line(5+ismon,(16*(i))+ismon,5+(8*(2+#proc.m.procs[i].name))+ismon,(16*(i))+ismon,0xFFFF)
            end
        end
        gfx.text(5,16*(1+#proc.m.procs),"Press key3 to Kill or Key2 to terminate",0xFFFF)
        gfx.text(5,16*(2+#proc.m.procs),"Key1 to close",0xFFFF)
        gfx.push()
        local y = 16*(3+#proc.m.procs)
        
    end
    local t = require("touch")
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
            if t.touching then
                cursor.set(t.x,t.y)
            end
            drawProcess()
            proc:delay(33.333)
            local buttons = button.get()
            if lastbutton[1] ~=buttons[1] or lastbutton[2] ~= buttons[2] or lastbutton[3] ~= buttons[3] then
            if buttons[1]==1 then  
                proc.signal = proc.m.SIG_TERM
            elseif buttons[2]==1 then
                local cx,cy = cursor.pos()
                local col = math.floor(cy/16)
                if proc.m.procs[col].name == proc.name then
                    giveFC(key)
                end
                proc.m.procs[col].signal = proc.m.SIG_TERM
            elseif buttons[3]==1 then
                local cx,cy = cursor.pos()
                local col = math.floor(cy/16)
                if proc.m.procs[col].name == proc.name then
                    giveFC(key)
                end
                proc.m.procs[col].signal = proc.m.SIG_KILL
                proc.m.procs[col].status = proc.m.PROCESS_DEAD
            end end
            lastbutton = button.get()
        else
            -- bg
            proc:delay(16.667)
        end
    end
    giveFC(key)
end