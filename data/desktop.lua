local VectorEngine = require "VectorEngine"

local using = false
local key = 0
local dirty = true

_G.clearKEY = function()
    dirty = true
    key = math.random(0, 1)
    using = false
end

---@type Process?
local lastproc = nil

---@type table<string, boolean>
local inbg = {}

---@type table<string, Process>
local gfxProc = {}

---@param proc Process
_G.requestFC = function(proc)
    -- จำ process ล่าสุดที่ขอ foreground
    gfxProc[proc.name] = proc

    -- process ที่เป็น background ไม่มีสิทธิ์ขอ FC
    if proc.runState == "background" then
        return false
    end

    if not using then
        lastproc = proc
        using = true
        key = math.random(0, 1)
        return true, key
    end

    return false
end

_G.giveFC = function(keey,overriddeDead)
    if not overriddeDead then
    if key ~= keey then
        return false
    end

    if lastproc then
        if lastproc.runState == "background" then
            inbg[lastproc.name] = true
        else
            inbg[lastproc.name] = false
        end
    end
    end
    FILL(0)
    dirty = true

    using = false
    key = 0

    return true
end


local logo = {
    ["wifi"] =
    "\x56\x47\x31\x03\xAD\x00\x2D\x01\x30\x00\x39\x00\xFF\xFF\x03\x7F\x00\xF7\x00\x8B\x00\x24\x00\xFF\xFF\x03\x5E\x00\xC2\x00\xD4\x00\x20\x00\xFF\xFF\x03\x3D\x00\x95\x00\x15\x01\x1E\x00\xFF\xFF\x02\x17\x00\x12\x00\x64\x01\x6C\x01\xFF\xFF\x06\x01\x17\x00\x14\x00\x63\x01\x68\x01",

    ["Dummy 1"] =
    "\x56\x47\x31\x02\x02\x00\x02\x00\x8C\x01\x8D\x01\x1F\xA0\x06\x01\x02\x00\x03\x00\x8C\x01\x8C\x01\x03\x2E\x00\x62\x01\x31\x01\x16\x00\xFF\xFF\x03\x29\x00\x3B\x01\x21\x00\x1D\x00\xFF\xFF\x03\x53\x00\x3D\x01\x1F\x00\x17\x00\xFF\xFF\x03\x88\x00\x38\x01\x2B\x00\x1B\x00\xFF\xFF\x03\xC6\x00\x35\x01\x28\x00\x1B\x00\xFF\xFF\x03\x06\x01\x2F\x01\x2A\x00\x20\x00\xFF\xFF\x03\x3E\x01\x2B\x01\x48\x00\x20\x00\xFF\xFF\x03\x5E\x01\x02\x01\x1C\x00\x1D\x00\xFF\xFF\x03\x24\x01\x04\x01\x22\x00\x14\x00\xFF\xFF\x03\xF0\x00\x0A\x01\x23\x00\x1E\x00\xFF\xFF\x03\xC1\x00\x0D\x01\x21\x00\x1E\x00\xFF\xFF\x03\x86\x00\x0D\x01\x28\x00\x19\x00\xFF\xFF\x03\x52\x00\x0C\x01\x25\x00\x22\x00\xFF\xFF\x03\x28\x00\x0F\x01\x22\x00\x1C\x00\xFF\xFF\x03\x2D\x00\xE6\x00\x25\x00\x1D\x00\xFF\xFF\x03\x5F\x00\xE3\x00\x21\x00\x1B\x00\xFF\xFF\x03\x87\x00\xE3\x00\x29\x00\x22\x00\xFF\xFF\x03\xB4\x00\xE2\x00\x26\x00\x24\x00\xFF\xFF\x03\xEC\x00\xDF\x00\x25\x00\x22\x00\xFF\xFF\x03\x1A\x01\xE2\x00\x30\x00\x16\x00\xFF\xFF\x03\x46\x01\xE7\x00\x1D\x00\x11\x00\xFF\xFF\x03\x24\x00\x1D\x00\x1A\x00\xA2\x00\xFF\xFF\x03\xDF\x00\x28\x00\x13\x00\x92\x00\xFF\xFF\x03\xEB\x00\x2E\x00\x45\x00\x16\x00\xFF\xFF\x03\x1C\x01\x44\x00\x1A\x00\x3E\x00\xFF\xFF\x03\xE6\x00\x79\x00\x3E\x00\x0B\x00\xFF\xFF\x03\xEB\x00\xAF\x00\x4A\x00\x0A\x00\xFF\xFF\x03\x29\x01\x80\x00\x0B\x00\x2F\x00\xFF\xFF\x03\x39\x00\x5C\x00\x1D\x00\x0B\x00\xFF\xFF\x03\x55\x00\x4C\x00\x19\x00\x18\x00\xFF\xFF\x03\x6D\x00\x2A\x00\x15\x00\x29\x00\xFF\xFF\x03\x7D\x00\x2C\x00\x11\x00\x05\x00\xFF\xFF\x03\x3B\x00\x63\x00\x20\x00\x18\x00\xFF\xFF\x03\x51\x00\x6F\x00\x17\x00\x1C\x00\xFF\xFF\x03\x58\x00\x7C\x00\x1E\x00\x1E\x00\xFF\xFF\x03\x69\x00\x8C\x00\x11\x00\x21\x00\xFF\xFF\x03\x71\x00\xA3\x00\x09\x00\x19\x00\xFF\xFF",

    ["TaskManager"] =
    "\x56\x47\x31\x02\x03\x00\x04\x00\x8B\x01\x89\x01\xF9\x07\x06\x01\x04\x00\x03\x00\x8A\x01\x89\x01\x03\x3C\x00\x2F\x00\x31\x01\xBB\x00\xF9\x07\x03\xC4\x00\x0C\x01\x20\x00\x4B\x00\xF9\x07\x03\x4B\x00\x37\x01\x14\x01\x2C\x00\xF9\x07\x03\x4D\x00\x44\x00\xCB\x00\x13\x00\x00\x00\x03\x4F\x00\x67\x00\x9E\x00\x13\x00\x00\x00\x03\x55\x00\x8E\x00\x40\x00\x11\x00\x00\x00\x03\x54\x00\x8F\x00\xF2\x00\x1A\x00\x00\x00\x03\x56\x00\xC1\x00\xB3\x00\x13\x00\x00\x00",

    ["snake"] =
    "\x56\x47\x31\x03\x46\x00\x33\x00\xF9\x00\x1D\x00\x84\x06\x03\x2E\x01\x4C\x00\x22\x00\x6C\x00\x84\x06\x03\x58\x00\xB0\x00\xE7\x00\x1F\x00\x84\x06\x03\x46\x00\xC2\x00\x21\x00\x93\x00\x84\x06\x03\x5E\x00\x41\x01\xCA\x00\x1E\x00\x84\x06\x03\x1F\x01\x21\x01\x1B\x00\x2A\x00\x84\x06\x05\x3A\x01\x49\x00\x18\x00\x84\x06\x05\x5A\x00\xC4\x00\x15\x00\x84\x06\x05\x3A\x01\xB7\x00\x18\x00\x84\x06\x05\x5B\x00\x4D\x01\x16\x00\x84\x06\x05\x27\x01\x4B\x01\x14\x00\x84\x06\x05\x4D\x00\x35\x00\x17\x00\x84\x06\x05\x4E\x00\x4F\x00\x15\x00\x84\x06\x05\x38\x00\x42\x00\x1F\x00\x84\x06\x05\x36\x00\x35\x00\x04\x00\x00\x00\x05\x35\x00\x52\x00\x05\x00\x00\x00\x06\x01\x02\x00\x03\x00\x8D\x01\x8C\x01"
}

local prog = {
    "wifi",
    "setting",
    "touchh",
    "TaskManager",
    "Dummy 1",
    "Dummy 2",
    "Dummy 3",
    "Dummy 4",
    "snake"
}

local nonelogo =
"\x56\x47\x31\x03\x3A\x00\x4B\x00\x04\x01\x25\x00\xFF\xFF\x03\xFF\x00\x4D\x00\x3E\x00\xB6\x00\xFF\xFF\x03\x83\x00\xD8\x00\xAE\x00\x27\x00\xFF\xFF\x03\x7E\x00\xE9\x00\x3E\x00\x4B\x00\xFF\xFF\x03\x8A\x00\x50\x01\x28\x00\x17\x00\xFF\xFF\x02\x03\x00\x03\x00\x8A\x01\x88\x01\x00\xF8"


---@param proc Process
return function(proc)
    local process = require("process")

    local function index(i)
        return math.floor((i - 1) / 3), math.floor((i - 1) % 3)
    end

    local function formatString(str)
        if #str > 9 then
            return str:sub(1, 6) .. "..."
        end

        return str
    end


    ----------------------------------------------------------------
    -- วาดรายการโปรแกรม
    ----------------------------------------------------------------

    ----------------------------------------------------------------
    -- วาดรายการโปรแกรม (แก้ไขให้ตรวจสอบสถานะและเคลียร์ Process ที่ตายแล้ว)
    ----------------------------------------------------------------

    local function drawEntry()
        dirty = false

        local xto = { 5 }

        FILL(0)

        local inbgprocss = 0

        -- ตรวจสอบและอัปเดตสถานะ background จาก gfxProc โดยตรง
        for name, proc in pairs(gfxProc) do
            if proc and proc.status ~= process.PROCESS_DEAD and proc.status ~= process.PROCESS_STOPPED then
                if proc.runState == "background" then
                    inbgprocss = inbgprocss + 1
                    inbg[name] = true
                else
                    inbg[name] = false
                end
            else
                -- เคลียร์ Process ที่ตายหรือหยุดทำงานแล้วออกจากระบบ
                gfxProc[name] = nil
                inbg[name] = nil
            end
        end

        if inbgprocss ~= 0 then
            gfx.text(
                5,
                210,
                "There are " .. tostring(inbgprocss) .. " in Background",
                0xFFFF
            )
        end

        for _, vv in ipairs(prog) do
            local x, y = index(_)
            local v = formatString(vv)

            if not xto[x + 1] then
                xto[x + 1] = 3
            end

            local sx = (x * 60) + xto[x + 1]
            local sy = y * 68

            local textPixelX = 8 * #v
            local offsetX = textPixelX - 60

            if offsetX > 0 then
                if not xto[x + 2] then
                    xto[x + 2] = 3
                end

                xto[x + 2] = xto[x + 2] + offsetX
            end

            VectorEngine.draw(
                sx,
                sy,
                logo[vv] or nonelogo,
                50,
                50
            )

            gfx.text(
                sx,
                sy + 50,
                v,
                0xFF00
            )

            gfx.push()
            proc:delay(1)
        end
    end


    ----------------------------------------------------------------
    -- หา AABB ของ icon
    ----------------------------------------------------------------

    local function getAABB(k)
        local xto = { 5 }

        for kk, vv in ipairs(prog) do
            local x, y = index(kk)
            local v = formatString(vv)

            if not xto[x + 1] then
                xto[x + 1] = 3
            end

            local textPixelX = 8 * #v
            local offsetX = textPixelX - 60

            if offsetX > 0 then
                if not xto[x + 2] then
                    xto[x + 2] = 3
                end

                xto[x + 2] = xto[x + 2] + offsetX
            end
        end

        local x, y = index(k)
        local v = prog[k]

        local sx = (x * 60) + xto[x + 1]
        local sy = y * 68

        if not logo[v] then
            return { sx, sy, 50, 50 }
        end

        return table.pack(
            VectorEngine.getAABB(
                sx,
                sy,
                logo[v] or nonelogo,
                1,
                50,
                50
            )
        )
    end


    ----------------------------------------------------------------
    -- AABB collision
    ----------------------------------------------------------------

    local function checkaa(tbl, x, y)
        local xa =
            x > tbl[1] and
            x < (tbl[1] + tbl[3])

        local xy =
            y > tbl[2] and
            y < (tbl[2] + tbl[4])

        return xa and xy
    end


    ----------------------------------------------------------------
    -- เปิดโปรแกรม
    ----------------------------------------------------------------


    local function launchProgram(programName)
        local procname = "DESKTOP-" .. programName
        local path = "/fs/prog/" .. programName .. ".lua"

        local oldproc = gfxProc[procname]

        if oldproc then
            -- Process เดิมยังมีชีวิตอยู่
            if oldproc.status ~= process.PROCESS_DEAD
                and oldproc.status ~= process.PROCESS_STOPPED
            then
                -- ถ้าอยู่ background ให้เอากลับมา foreground
                if oldproc.runState == "background" then
                    inbg[procname] = false
                    oldproc.runState = "foreground"
                    --dirty = true
                    return true
                end

                -- อยู่ foreground และยังทำงานอยู่
                return false
            end

            -- Process เดิมตาย/หยุดแล้ว
            gfxProc[procname] = nil
            inbg[procname] = nil
        end

        local code = readfile(path)

        if not code then
            return nil, "NOFILE"
        end

        local newproc = process.create(
            procname,
            path,
            false
        )

        gfxProc[procname] = newproc
        inbg[procname] = false

        dirty = true

        return true
    end




    ----------------------------------------------------------------
    -- Initial draw
    ----------------------------------------------------------------

    drawEntry()

    local t = require("touch")

    local inb
    local tourond = false


    ----------------------------------------------------------------
    -- Main loop
    ----------------------------------------------------------------

    while true do
        if dirty and (not using) then
            drawEntry()
        end


        if not using then
            if cursor.isDown() == 0
                or (t.touching and not t.holding)
            then
                local calculatedONmouse = false


                for i = 1, #prog do
                    ------------------------------------------------
                    -- Mouse
                    ------------------------------------------------

                    if cursor.isDown() == 1 then
                        tourond = true
                        goto backtoup
                    end

                    tourond = false
                    calculatedONmouse = true

                    inb =
                        checkaa(
                            getAABB(i),
                            cursor.pos()
                        )

                    goto startch


                    ------------------------------------------------
                    -- Touch
                    ------------------------------------------------

                    ::backtoup::

                    tourond = true

                    if not t.touching
                        or calculatedONmouse
                    then
                        goto endend
                    end

                    inb =
                        checkaa(
                            getAABB(i),
                            t.x,
                            t.y
                        )


                    ::startch::


                    if inb then
                        local programName = prog[i]
                        local path =
                            "/fs/prog/" ..
                            programName ..
                            ".lua"


                        ------------------------------------------------
                        -- ตรวจไฟล์
                        ------------------------------------------------

                        if not readfile(path) then
                            local text = "No program file"
                            local text2 = " Press any key "

                            local l =
                                math.floor((#text * 8) / 2)

                            gfx.rect(
                                88,
                                99,
                                241 - 88,
                                146 - 99,
                                gfx.rgb(0x5a5a5a),
                                true
                            )

                            gfx.text(
                                math.floor(320 / 2) - l,
                                102,
                                text,
                                0xFFFF,
                                gfx.rgb(0x5a5a5a)
                            )

                            gfx.text(
                                math.floor(320 / 2) - l,
                                102 + 16,
                                text2,
                                0xFFFF,
                                gfx.rgb(0x5a5a5a)
                            )

                            gfx.push()

                            local _, ch =
                                proc:poll({
                                    0, 1, 2, 3, 4
                                })

                            proc:delay(16)

                            if not ch then
                                proc:poll({
                                    0, 1, 2, 3, 4
                                })
                            end

                            FILL(0)
                            dirty = true
                        else
                            ------------------------------------------------
                            -- เปิดโปรแกรม
                            ------------------------------------------------

                            local result, err =
                                launchProgram(programName)

                            ------------------------------------------------
                            -- ถ้า process เดิมถูกเปิดกลับจาก background
                            -- หรือสร้างใหม่สำเร็จ
                            ------------------------------------------------

                            if result then
                                dirty = true
                            end
                        end
                    end


                    ::continue::

                    if not tourond then
                        goto backtoup
                    end


                    ::endend::
                end
            end
        end


        proc:delay(16.667)
    end
end
