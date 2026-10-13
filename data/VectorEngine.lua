local VectorEngine = {}

-- ฟังก์ชันช่วยอ่าน 16-bit Signed Integer (Little-Endian)
local function readInt16(data, pos)
    local lo, hi = string.byte(data, pos, pos + 1)
    if not lo or not hi then return 0 end
    local num = lo + hi * 256
    if num >= 32768 then
        num = num - 65536
    end
    return num
end

-- ฟังก์ชันช่วยอ่าน 16-bit Unsigned Integer (Little-Endian)
local function readUint16(data, pos)
    local lo, hi = string.byte(data, pos, pos + 1)
    if not lo or not hi then return 0 end
    return lo + hi * 256
end

-- ปัดเศษ
local function round(n)
    return math.floor(n + 0.5)
end

--- วาดภาพจาก Data (เป็น Byte จริงๆ) โดยกำหนด Pixel W, H
---@param x integer Origin X
---@param y integer Origin Y
---@param data string Binary Bytes (ใช้ io.read("*a") จาก .bin หรือพิมพ์ "\x56\x47..." ตรงๆ)
---@param target_w integer ความกว้างเป้าหมาย (Pixel)
---@param target_h integer ความสูงเป้าหมาย (Pixel)
---@param orig_w? integer กว้างต้นฉบับ (ค่าเริ่มต้น = 400)
---@param orig_h? integer สูงต้นฉบับ (ค่าเริ่มต้น = 400)
function VectorEngine.draw(x, y, data, target_w, target_h, orig_w, orig_h)
    orig_w = orig_w or 400
    orig_h = orig_h or 400
    local sx = target_w / orig_w
    local sy = target_h / orig_h
    local sr = (sx + sy) / 2 

    -- เช็ค Header "VG1" (Byte: 86, 71, 49)
    if string.sub(data, 1, 3) ~= "VG1" then
        print("Invalid Binary Vector Data")
        return
    end

    local pos = 4 -- เริ่มที่ Byte ที่ 4
    local dataLen = #data

    while pos <= dataLen do
        local type = string.byte(data, pos)
        pos = pos + 1

        if type == 1 then -- Line
            local x1 = readInt16(data, pos)
            local y1 = readInt16(data, pos + 2)
            local x2 = readInt16(data, pos + 4)
            local y2 = readInt16(data, pos + 6)
            local color = readUint16(data, pos + 8)
            pos = pos + 10
            
            gfx.line(x + round(x1 * sx), y + round(y1 * sy), 
                     x + round(x2 * sx), y + round(y2 * sy), color)

        elseif type == 2 or type == 3 then -- Rect / RectFill
            local rx = readInt16(data, pos)
            local ry = readInt16(data, pos + 2)
            local w = readUint16(data, pos + 4)
            local h = readUint16(data, pos + 6)
            local color = readUint16(data, pos + 8)
            local filled = (type == 3)
            pos = pos + 10
            
            gfx.rect(x + round(rx * sx), y + round(ry * sy), 
                     round(w * sx), round(h * sy), color, filled)

        elseif type == 4 or type == 5 then -- Circle / CircleFill
            local cx = readInt16(data, pos)
            local cy = readInt16(data, pos + 2)
            local r = readUint16(data, pos + 4)
            local color = readUint16(data, pos + 6)
            local filled = (type == 5)
            pos = pos + 8
            
            gfx.circle(x + round(cx * sx), y + round(cy * sy), round(r * sr), color, filled)

        elseif type == 6 then -- AABB Hitbox
            pos = pos + 9 -- Skip: id(1) + x(2) + y(2) + w(2) + h(2)
        else
            break
        end
    end
    gfx.push()
end

--- ดึงข้อมูล AABB จาก Binary Data
function VectorEngine.getAABB(x, y, data, target_id, target_w, target_h, orig_w, orig_h)
    target_id = target_id or 1
    orig_w = orig_w or 400
    orig_h = orig_h or 400
    local sx = target_w / orig_w
    local sy = target_h / orig_h

    if string.sub(data, 1, 3) ~= "VG1" then return nil end

    local pos = 4
    while pos <= #data do
        local type = string.byte(data, pos)
        pos = pos + 1

        if type == 1 then pos = pos + 10
        elseif type == 2 or type == 3 then pos = pos + 10
        elseif type == 4 or type == 5 then pos = pos + 8
        elseif type == 6 then
            local id = string.byte(data, pos)
            if id == target_id then
                local rx = readInt16(data, pos + 1)
                local ry = readInt16(data, pos + 3)
                local w = readUint16(data, pos + 5)
                local h = readUint16(data, pos + 7)
                return x + round(rx * sx), y + round(ry * sy), round(w * sx), round(h * sy)
            end
            pos = pos + 9
        else break end
    end
    return nil
end

return VectorEngine