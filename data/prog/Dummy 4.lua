local function smoothVU(current, target)
    local diff = math.abs(target - current)
    local speed = math.min(1, math.max(0.03, diff * 3))
    return current + (target - current) * speed
end

local function drawGradient(startX, startY, w, h, ww)
    local total = math.max(1, ww - 1)

    for x = 0, w - 20 do
        local t = x / total
        local r, g

        if t < 0.5 then
            -- เขียว -> เหลือง
            r = math.floor(255 * (t * 2))
            g = 255
        else
            -- เหลือง -> แดง
            r = 255
            g = math.floor(255 * (1 - (t - 0.5) * 2))
        end

        local rgb888 = (r << 16) | (g << 8)
        local color = gfx.rgb(rgb888)

        gfx.rect(startX + x, startY, 1, h, color, true)
    end
end

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

    -- แสดงเวลาเป็น mm:ss
    local function fmt_time(ms)
        local s = math.floor((ms or 0) / 1000)
        return string.format("%02d:%02d", math.floor(s / 60), s % 60)
    end
    local timeText = "00:00"
    local nextUpdate = 0
    local vl,vr = 0,0
    local nextime = time.millis()
    cursor.visible(false)
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
            cursor.visible(not ok)
            if ok then isOwnedKey = true end
        end
        if proc:canDraw() then
            FILL(0)
            local l,r = audio.getVU()

        vl = smoothVU(vl, l)
        vr = smoothVU(vr, r)
            gfx.rect(0, 0, math.floor(volume * 320), 240, 0xFD55, true)
            --gfx.rect(0,100,math.floor(320*vl),16,0xFD00,true)
            --gfx.rect(0,120,math.floor(320*vr),16,0xFD00,true)

            local rl = math.floor((vl+0.1)*320)
            local rr = math.floor((vr+0.1)*320)
            --print(rl,rr,vl,vr)
           -- gfx.rect(rl,100,320-rl,16,0,true)
           -- gfx.rect(rr,120,320-rr,16,0,true)
           gfx.rect(0,80,320,80,0,true)
            gfx.rect(22,90,10,70,0xFD00,true)
            drawGradient(0,100,rl,16,320)
            drawGradient(0,120,rr,16,320)
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
                cursor.visible(true)
                proc.signal = proc.m.SIG_TERM
            elseif bts[3] == 1 then
                proc.runState = "background"
                cursor.visible(true)
            elseif bts[2] == 1 then
                local status = audio.status()
                print(status.error)
                local url = keyboard(proc,"file://","file://")
                
                if not url:lower():match("%.mp3$") then
                    url = url .. ".mp3"
                end
                print(url)
                audio.play(url)
                audio.resume()
                cursor.visible(false)
            end
            proc:delay(33.3333)
        else
            proc:delay(16.667)
        end
    end
    giveFC(key)
end
