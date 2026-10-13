
local VectorEngine = require "VectorEngine"
local sprite       = require "sprite"
local at           = require "at"

---@param proc Process
return function(proc)
    local ok, key = requestFC(proc)
    if not ok then
        error("cannot request full screen")
    end

    local isOwnedKey = true
    local bx, by = 5, 50
    local t = require "touch"
    local connect = false

    local xx, yy, ww, hh =
        VectorEngine.getAABB(bx, by, sprite["decline"], 1, 50, 50)

    -- ตรวจ signal และตรวจว่ายังทำงานต่อได้หรือไม่
    local function alive()
        local sig = proc.m.receiveSignal(proc)

        if sig ~= proc.m.SIG_NON
            or proc.signal == proc.m.SIG_TERM
            or proc.signal == proc.m.SIG_KILL then
            proc.status = proc.m.PROCESS_DEAD
            return false
        end

        return true
    end

    -- รอแตะปุ่ม โดยไม่ใช้ repeat-until
    local function waitTouch()
        while alive() do
            if not proc:canDraw() then
                return false
            end
                gfx.text(
                    5, 5,
                    "Wifi Status: " ..
                    tostring(wifi.sta.status().connected),
                    0xFFFF
                )
                gfx.text(
                    5, 30,
                    "Ip: " .. (wifi.sta.ip() or "NO ip"),
                    0xFFFF
                )
                gfx.push()
            local bts = button.get()

            if bts[1] == 1 then
                proc.runState = "background"
                return false
            end

            if t.down
                and t.x > xx and t.x < (xx + ww)
                and t.y > yy and t.y < (yy + hh) then
                return true
            end

            proc:delay(33.3333)
        end

        return false
    end

    -- รอ Wi-Fi โดยตรวจ signal และ foreground
    local function waitWifi()
        local frame = 0
        local maxFrame = 166
        while alive() do
            if not proc:canDraw() then
                return false
            end
            frame = frame+1
            if wifi.sta.status().connected == true then
                return true
            end
            if frame >=maxFrame then
                return false
            end
            proc:delay(33.333)
        end

        return false
    end

    while alive() do
        local bg = not proc:canDraw()

        if bg then
            if isOwnedKey then
                giveFC(key)
                isOwnedKey = false
            end

            proc:delay(16.667)
        else
            if not isOwnedKey then
                ok, key = requestFC(proc)
                if not ok then
                    proc:delay(16.667)
                else
                    isOwnedKey = true
                end
            end

            if isOwnedKey and proc:canDraw() then
                FILL(0)
                gfx.text(
                    5, 5,
                    "Wifi Status: " ..
                    tostring(wifi.sta.status().connected),
                    0xFFFF
                )
                gfx.text(
                    5, 30,
                    "Ip: " .. (wifi.sta.ip() or "NO ip"),
                    0xFFFF
                )
                gfx.push()
                if not connect and wifi.sta.ip() then
                    connect = true
                end
                if not connect then
                    VectorEngine.draw(
                        bx, by, sprite["accept"], 50, 50
                    )

                    if waitTouch() then
                        connect = true

                        VectorEngine.draw(
                            bx, by, sprite["Info"], 50, 50
                        )
                        gfx.push()

                        local ssid = keyboard(proc, "", "SSID")
                        local password =
                            keyboard(proc, "", "password")

                        if not alive() or not proc:canDraw() then
                            connect = false
                        else
                            FILL(0)
                            gfx.push()

                            wifi.mode("sta")
                            wifi.sta.config({
                                ssid = ssid,
                                password = password
                            })
                            collectgarbage("collect")
                            wifi.start()

                            if not waitWifi() then
                                connect = false
                            end

                            gfx.push()
                        end
                    end
                else
                    VectorEngine.draw(
                        bx, by, sprite["decline"], 50, 50
                    )

                    if waitTouch() then
                        wifi.sta.disconnect()
                        connect = false
                    end
                end
            else
                proc:delay(16.667)
            end
        end
    end

    if isOwnedKey then
        giveFC(key)
    end
end
