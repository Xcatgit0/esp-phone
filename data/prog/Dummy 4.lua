---@param proc Process
return function(proc)
    local ok, key = requestFC(proc)
    local isOwnedKey = true
    if not ok then error("cannot request full screen") end
    local bg = false

    audio.mono(false)
    
    audio.volume(0.5)
    local volume = 0.5
    local t = require("touch")
    collectgarbage("collect")
    print("free", memory.free())

    local ok2, err = audio.play("http://192.168.1.8:8080/lu.mp3", { loop = true })
    print("play:", ok2, err)

    -- แสดงเวลาเป็น mm:ss
    local function fmt_time(ms)
        local s = math.floor((ms or 0) / 1000)
        return string.format("%02d:%02d", math.floor(s / 60), s % 60)
    end
    local timeText = "00:00"
    local nextUpdate = 0

    while (proc.m.receiveSignal(proc) == proc.m.SIG_NON) do
        if proc.signal == proc.m.SIG_TERM or proc.signal == proc.m.SIG_KILL then
            proc.status = proc.m.PROCESS_DEAD
            coroutine.yield()
        end
        bg = not proc:canDraw()
        if bg and isOwnedKey then
            giveFC(key)
            isOwnedKey = false
        elseif not bg and not isOwnedKey then
            ok, key = requestFC(proc)
            if ok then isOwnedKey = true end
        end
        if proc:canDraw() then
            FILL(0)
            gfx.rect(0, 0, math.floor(volume * 320), 240, 0xFD55, true)

            if t.touching then
                volume = t.x / 320
                audio.volume(volume)
            end

            -- อัปเดตข้อความ 4 ครั้ง/วินาที
            local now = time.millis()
            if now >= nextUpdate then
                nextUpdate = now + 250
                local st = audio.status()
                timeText = string.format("%s  %s  buf %dK",
                    fmt_time(st.position), tostring(st.state), math.floor((st.buffered or 0) / 1024))
                if st.error then timeText = timeText .. "  ERR: " .. tostring(st.error) end
            end
            gfx.text(8, 8, timeText, 0xFFFF, 0x0000)

            gfx.push()

            local bts = button.get()
            if bts[1] == 1 then
                audio.stop()
                proc.signal = proc.m.SIG_TERM
            elseif bts[3] == 1 then
                proc.runState = "background"
            end
            proc:delay(16.667)
        else
            proc:delay(16.667)
        end
    end
    giveFC(key)
end