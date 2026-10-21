-- "/fs/prog/wifi.lua" -> "/fs/prog"    ("/fs" -> "/",  "/" -> "/")
local function dirname(path)
    local d = path:match("^(.*)/[^/]*/*$")
    if d == nil then return "." end
    return d == "" and "/" or d
end
-- "/fs/prog/wifi.lua" -> "wifi.lua"
local function basename(path)
    return path:match("([^/]*)/*$") or path
end
-- "wifi.lua" -> "wifi", "lua"
local function splitext(name)
    local stem, ext = name:match("^(.*)%.([^./]+)$")
    if not stem or stem == "" then return name, "" end    -- ".bashrc" หรือไม่มีนามสกุล
    return stem, ext
end
local totalfile = 0
local totaldir = 0
---@class IndexEntry
---@field name string   -- path เต็ม
---@field type "file"|"dir"
---@field size integer
---@field mtime integer
---@field nested integer

---@param tbl IndexEntry[]
---@param path string
---@param nested integer
local function recursives(tbl, path, nested)
    local sta = fs.stat(path)
    if not sta then return end
    --print(path)
    tbl[#tbl + 1] = {
        name = path, type = sta.type, size = sta.size,
        mtime = sta.mtime, nested = nested + 1,
    }
    if sta.type=="file" then totalfile = totalfile +1 end
    if sta.type == "dir" then
        totaldir = totaldir + 1
        local list = fs.list(path)
        if not list then return end
        if nested >5 then return end
        for _, v in ipairs(list) do
            recursives(tbl, fs.join(path, v.name), nested + 1)
        end
    end
end

---@param proc Process
return function(proc)
    ---@type IndexEntry[]
    local index = {}
    local tbl = index
    local usbAvailable = fs.isdir("/usb")
    --recursives(index, "/", 0)
    if not _G.usbINDEX then recursives(index, "/usb", 0) else index = _G.usbINDEX end
    if not _G.usbINDEX then
        _G.usbINDEX = index
    end
    if not usbAvailable then _G.usbINDEX = nil end
    --if usbAvailable then recursives(index, "/usb", 0) end
    print(string.format("Total Directory: %d , Total file: %d",totaldir,totalfile))
    local offsetY =16
    local itemHeight = 16
    local screenHeight = 320 -- ความสูงหน้าจอ
    local t = require("touch")
    while true do
        FILL(0)
        if t.touching then
            cursor.set(t.x,t.y)
            cursor.visible(false)
        else
        end
        local btn = button.get()
        local _,cursorX = cursor.pos()
        local ke = math.floor((cursorX - offsetY) / itemHeight) + 1
        -- รับปุ่มกดเลื่อน
        if btn[1] == 1 then
            offsetY = offsetY + 8
        end
        if btn[2] == 1 then
            offsetY = offsetY - 8
        end
        if btn[3] == 1 then
            cursor.visible(true)
            return index[ke]
        end
        local str = "Not selected"
        if ke~= 0 and index[ke] then str = index[ke].name end
        
        
        -- คำนวณขอบเขตการเลื่อนไม่ให้หลุดหายไปในที่ว่าง
        local totalHeight = #index * itemHeight
        local maxOffsetY = 16
        local minOffsetY = screenHeight - totalHeight
        if minOffsetY > 0 then minOffsetY = 0 end

        if offsetY > maxOffsetY then offsetY = maxOffsetY end
        if offsetY < minOffsetY then offsetY = minOffsetY end

        -- วาดรายการทั้งหมด
        for k, vv in ipairs(index) do
            local y = offsetY + (k - 1) * itemHeight
            local x = 16 * vv.nested

            -- 1. วาดเส้นแนวตั้งของโฟลเดอร์ (แยกมาเช็คต่างหาก เพื่อให้แสดงผลแม้ Parent จะหลุดจอไปแล้ว)
            if vv.type == "dir" then
                local lastChildIndex = k
                for i = k + 1, #index do
                    if index[i].nested <= vv.nested then
                        break
                    end
                    lastChildIndex = i
                end
                
                if lastChildIndex > k then
                    local startY = y + 8
                    local endY = offsetY + (lastChildIndex - 1) * itemHeight + 8
                    
                    -- ถ้าระยะของเส้นแนวตั้งอยู่ในขอบเขตจอ (ตัดผ่านหน้าจอ) ให้วาด
                    if endY >= 0 and startY <= screenHeight then
                        gfx.line(x + 2, math.floor(startY), x + 2, math.floor(endY), 0xFFFF)
                    end
                end
            end

            -- 2. วาดส่วนของตัวไอเทมนั้นๆ (เช็คเฉพาะตอนที่ตัวมันเองอยู่บนจอ)
            if y > -itemHeight and y < screenHeight + itemHeight then
                -- เส้นแนวนอนเชื่อม
                --print("IN")
                if vv.nested > 0 then
                    local parentX = 16 * (vv.nested - 1)
                    gfx.line(parentX + 2, y + 8, x, y + 8, 0xFFFF)
                end

                -- ข้อความชื่อไฟล์/โฟลเดอร์
                gfx.text(x, y, basename(vv.name), 0xFFFF,(ke==k) and 0x3333 or 0)
            end
            --print("INOT")
        end

        -- วาด Scrollbar ทางขวาจอ
        if totalHeight > screenHeight then
            local scrollBarWidth = 4
            local screenWidth = 320 
            local barHeight = math.max(20, math.floor((screenHeight / totalHeight) * screenHeight))
            local barY = math.floor((-offsetY / totalHeight) * screenHeight)
            
            gfx.rect(screenWidth - 6, 0, scrollBarWidth, screenHeight, 0x3333,true)
            gfx.rect(screenWidth - 6, barY-32, scrollBarWidth, barHeight, 0xFFFF,true)
        end
        gfx.rect(0,0,320,16,0x3333,true)
        gfx.text(0,0,str,0xFFFF,0x3333)
        gfx.push()
        proc:delay(33.333)
    end
end