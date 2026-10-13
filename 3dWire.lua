-- wireframe_3d.lua
-- 320x240 Wireframe 3D renderer
-- หมุนรอบ X/Y/Z พร้อมกัน

local W = 320
local H = 240

local CX = W / 2
local CY = H / 2

-- =========================
-- 3D Cube
-- =========================

local vertices = {
    {-1, -1, -1},
    { 1, -1, -1},
    { 1,  1, -1},
    {-1,  1, -1},

    {-1, -1,  1},
    { 1, -1,  1},
    { 1,  1,  1},
    {-1,  1,  1},
}

local edges = {
    {1, 2},
    {2, 3},
    {3, 4},
    {4, 1},

    {5, 6},
    {6, 7},
    {7, 8},
    {8, 5},

    {1, 5},
    {2, 6},
    {3, 7},
    {4, 8},
}

-- =========================
-- Rotation
-- =========================

local function rotate(x, y, z, rx, ry, rz)

    -- X rotation
    local cy = math.cos(rx)
    local sy = math.sin(rx)

    local y1 = y * cy - z * sy
    local z1 = y * sy + z * cy

    y = y1
    z = z1

    -- Y rotation
    local cx = math.cos(ry)
    local sx = math.sin(ry)

    local x1 = x * cx + z * sx
    local z2 = -x * sx + z * cx

    x = x1
    z = z2

    -- Z rotation
    local cz = math.cos(rz)
    local sz = math.sin(rz)

    local x2 = x * cz - y * sz
    local y2 = x * sz + y * cz

    return x2, y2, z
end

-- =========================
-- Perspective projection
-- =========================

local function project(x, y, z)

    -- ดัน object ออกจากกล้อง
    z = z + 5

    -- กันหารด้วย 0 / จุดที่อยู่หลังกกล้อง
    if z <= 0.1 then
        return nil
    end

    local fov = 120

    local scale = fov / z

    local sx = CX + x * scale
    local sy = CY - y * scale

    return math.floor(sx), math.floor(sy)
end

-- =========================
-- Render
-- =========================

local function render(rx, ry, rz)

    -- clear screen
    gfx.rect(0, 0, W, H, 0x0000, true)

    local projected = {}

    -- Transform + project ทุก vertex
    for i, v in ipairs(vertices) do

        local x, y, z =
            rotate(
                v[1],
                v[2],
                v[3],
                rx,
                ry,
                rz
            )

        local sx, sy = project(x, y, z)

        if sx then
            projected[i] = {
                x = sx,
                y = sy
            }
        end
    end

    -- วาด edges
    local white = gfx.rgb(0xFFFFFF)

    for _, edge in ipairs(edges) do

        local a = projected[edge[1]]
        local b = projected[edge[2]]

        if a and b then
            gfx.line(
                a.x,
                a.y,
                b.x,
                b.y,
                white
            )
        end
    end

    gfx.push()
end

-- =========================
-- Main animation
-- =========================

local start = time.millis()

while true do

    -- เวลาตั้งแต่เริ่ม
    local t = time.millis() - start

    -- ความเร็วแต่ละแกน
    local rx = t * 0.0010
    local ry = t * 0.0014
    local rz = t * 0.0008

    render(rx, ry, rz)

    -- ประมาณ 60 FPS
    delay(16)
end