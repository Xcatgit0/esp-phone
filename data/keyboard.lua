local browse = require "browse"
---@param proc Process
---@param text string?
function _G.keyboard(proc, text,holder)
    FILL(0)
    gfx.push()
    local shiftMap = {
        ["1"]="!", ["2"]="@", ["3"]="#", ["4"]="$", ["5"]="%", ["6"]="^",
        ["7"]="&", ["8"]="*", ["9"]="(", ["0"]=")",
        ["-"]="_", ["="]="+", ["["]="{", ["]"]="}", ["\\"]="|",
        [";"]=":", ["'"]="\"", [","]="<", ["."]=">", ["/"]="?"
    }

    local keys = {
        {" ","1","2","3","4","5","6","7","8","9","0","-","=","<---"},
        {"  ","q","w","e","r","t","y","u","i","o","p","[","]","\\"},
        {"CAPS","a","s","d","f","g","h","j","k","l",";","'","DONE"},
        {"SHFT","z","x","c","v","b","n","m",",",".","/","SHFT"},
        {"~","<",">"},
        {"BROWSE","http://192.168.1."}
    }

    text = text or ""

    -- cursor เป็นตำแหน่ง "ระหว่าง" ตัวอักษร
    -- 1 = ก่อนตัวแรก
    -- #text + 1 = หลังตัวสุดท้าย
    local pos = #text + 1

    local isShift = false
    local isCapsLock = false

    local startX = 6
    local startY = 90
    local keyW = 19
    local keyH = 22
    local gap = 2
    local hitPadding = 2

    local textColor = 0xffff
    local borderColor = 0xffff
    local activeColor = 0x07e0

    local buttons = {}

    for row, keysRow in ipairs(keys) do
        local x = startX
        local y = startY + (row - 1) * (keyH + gap)

        for _, keyName in ipairs(keysRow) do
            local w = keyW

            if keyName == "<---" then
                w = keyW * 2.2
            elseif keyName == "CAPS" then
                w = keyW * 1.8
            elseif keyName == "DONE" then
                w = keyW * 2.1
            elseif keyName == "SHFT" then
                w = keyW * 2.4
            elseif keyName == "~" then
                w = keyW * 13
            elseif keyName == " " then
                w = keyW * 0.65
            elseif keyName == "\\" then
                w = keyW * 1.7
            elseif keyName == "BROWSE" or keyName=="http://192.168.1." then
                w = keyW * 8
            end

            buttons[#buttons + 1] = {
                key = keyName,
                x = math.floor(x),
                y = y,
                w = math.floor(w),
                h = keyH
            }

            x = x + w + gap
        end
    end

    local function keyFromPosition(x, y)
        for _, button in ipairs(buttons) do
            if x >= button.x - hitPadding
                and x <= button.x + button.w + hitPadding
                and y >= button.y - hitPadding
                and y <= button.y + button.h + hitPadding
            then
                return button.key
            end
        end

        return nil
    end

    local function draw()
        FILL(0)

        -- ข้อความ
        gfx.text(10, 15, holder or "", gfx.rgb(0x555555))
        gfx.text(10, 15, text, textColor)

        -- cursor
        local cursorX = 10 + ((pos - 1) * 8)

        gfx.line(
            cursorX,
            14,
            cursorX,
            30,
            textColor
        )

        gfx.line(10, 40, 310, 40, borderColor)

        for _, button in ipairs(buttons) do
            local rawKey = button.key
            local displayText = rawKey

            local isUpperMode = isShift or isCapsLock

            if rawKey:match("^%a$") then
                displayText = isUpperMode
                    and rawKey:upper()
                    or rawKey:lower()

            elseif isShift and shiftMap[rawKey] then
                displayText = shiftMap[rawKey]

            elseif displayText == "~" then
                displayText = " "
            end

            local currentBorderColor = borderColor
            local currentTextColor = textColor

            if (rawKey == "SHFT" and isShift)
                or (rawKey == "CAPS" and isCapsLock)
            then
                currentBorderColor = activeColor
                currentTextColor = activeColor
            end

            gfx.rect(
                button.x,
                button.y,
                button.w,
                button.h,
                currentBorderColor,
                false
            )

            local textWidth = #displayText * 8

            local tx = button.x
                + math.floor((button.w - textWidth) / 2)

            local ty = button.y
                + math.floor((button.h - 16) / 2)

            gfx.text(
                tx,
                ty,
                displayText,
                currentTextColor
            )
        end

        gfx.push()
    end
    draw()

    local downed = false
    local t = require("touch")


    proc:delay(250)
    while true do
        local event = false
        local x, y

        if t.down and not downed then
            event = true
            x = t.x
            y = t.y
        end

        downed = t.down

        if event then
            local keyName = keyFromPosition(x, y)

            if keyName then

                ----------------------------------------------------
                -- BACKSPACE
                ----------------------------------------------------

                if keyName == "<---" then
                    if pos > 1 then
                        text =
                            text:sub(1, pos - 2)
                            .. text:sub(pos)

                        pos = pos - 1
                    end

                ----------------------------------------------------
                -- CURSOR LEFT
                ----------------------------------------------------

                elseif keyName == "<" then
                    if pos > 1 then
                        pos = pos - 1
                    end

                ----------------------------------------------------
                -- CURSOR RIGHT
                ----------------------------------------------------

                elseif keyName == ">" then
                    if pos <= #text then
                        pos = pos + 1
                    end

                ----------------------------------------------------
                -- CAPS LOCK
                ----------------------------------------------------

                elseif keyName == "CAPS" then
                    isCapsLock = not isCapsLock

                ----------------------------------------------------
                -- SHIFT
                ----------------------------------------------------

                elseif keyName == "SHFT" then
                    isShift = not isShift

                ----------------------------------------------------
                -- DONE
                ----------------------------------------------------

                elseif keyName == "DONE" then
                    break

                ----------------------------------------------------
                -- SPACE
                ----------------------------------------------------

                elseif keyName == "~" then
                    text =
                        text:sub(1, pos - 1)
                        .. " "
                        .. text:sub(pos)

                    pos = pos + 1

                    if isShift then
                        isShift = false
                    end

                ----------------------------------------------------
                -- EMPTY / SPECIAL SPACE KEY
                ----------------------------------------------------

                elseif keyName == " " or keyName == "  " then
                    -- ไม่ทำอะไร
                    -- เพราะ "~" เป็นปุ่ม space จริง
                    -- ส่วนสองปุ่มนี้ใช้เป็น layout spacer
                elseif keyName == "BROWSE" then
                    text = text .. browse(proc).name
                ----------------------------------------------------
                -- CHARACTER
                ----------------------------------------------------
                else
                    local charToAppend = keyName

                    local isUpperMode =
                        isShift or isCapsLock

                    if keyName:match("^%a$") then
                        charToAppend =
                            isUpperMode
                            and keyName:upper()
                            or keyName:lower()

                    elseif isShift and shiftMap[keyName] then
                        charToAppend = shiftMap[keyName]
                    end

                    -- แทรกตรง cursor
                    text =
                        text:sub(1, pos - 1)
                        .. charToAppend
                        .. text:sub(pos)

                    pos = pos + #charToAppend

                    -- Shift ใช้ครั้งเดียว
                    if isShift then
                        isShift = false
                    end
                end

                draw()
            end
        end

        proc:delay(10)
    end

    return text
end