-- bench_game.lua : วัดงบเวลาต่อเฟรมของเกม (วาดรูปหลายเหลี่ยม + physics) บน ESP32 จริง
-- รันเป็น bootstrap ชั่วคราว หรือ dofile("/fs/bench_game.lua")
-- หมายเหตุ: time.millis() ละเอียดแค่ 10 ms จึงวัดโดยรันซ้ำจนครบ MIN_MS แล้วหารเฉลี่ย

local MIN_MS = 400
local W, H = 320, 240
local sin, cos, sqrt, floor = math.sin, math.cos, math.sqrt, math.floor
local line, rect, push, rgb = gfx.line, gfx.rect, gfx.push, gfx.rgb

local function bench(fn)
    local n = 0
    local t0 = time.millis()
    local t1 = t0
    while t1 - t0 < MIN_MS do
        fn()
        n = n + 1
        t1 = time.millis()
    end
    return (t1 - t0) / n   -- ms ต่อครั้ง
end

local function fmt(ms) return string.format("%7.2f ms", ms) end
print("=== bench_game ===")
print("memory.free at start:", memory.free())

-- 1) ล้างจอเต็มจอ (gfx.rect filled) -------------------------------------------
local t_clear = bench(function() rect(0, 0, W, H, 0x0000, true) end)
print("full-screen clear (gfx.rect)  ", fmt(t_clear))

-- 2) gfx.line ความยาวเฉลี่ย ~24 px ----------------------------------------------
local white = 0xFFFF
local t_line100 = bench(function()
    for i = 1, 100 do line(10 + i, 20, 30 + i, 40, white) end   -- 100 เส้น ยาว ~28 px
end)
print("100 x gfx.line (~28 px each)  ", fmt(t_line100), string.format("(%.3f ms/line)", t_line100 / 100))

-- 3) รูปหลายเหลี่ยม: หมุน + วาดขอบ ----------------------------------------------
-- เก็บเป็น array ของตัวเลข (ไม่สร้าง table ใหม่ต่อเฟรม) เพื่อไม่ให้ GC ทำงาน
local function make_polys(P, V)
    local px, py, ang, spin, rad = {}, {}, {}, {}, {}
    for i = 1, P do
        px[i] = math.random(20, W - 20); py[i] = math.random(20, H - 20)
        ang[i] = math.random() * 6.283; spin[i] = (math.random() - 0.5) * 0.1
        rad[i] = 6 + math.random() * 6
    end
    return px, py, ang, spin, rad
end

local function poly_frame(P, V, px, py, ang, spin, rad, draw)
    local step = 6.2831853 / V
    for i = 1, P do
        local a = ang[i] + spin[i]
        ang[i] = a
        local cx, cy, r = px[i], py[i], rad[i]
        local fx, fy = cx + cos(a) * r, cy + sin(a) * r
        local lx, ly = fx, fy
        for k = 1, V - 1 do
            local b = a + k * step
            local nx, ny = cx + cos(b) * r, cy + sin(b) * r
            if draw then line(floor(lx), floor(ly), floor(nx), floor(ny), white) end
            lx, ly = nx, ny
        end
        if draw then line(floor(lx), floor(ly), floor(fx), floor(fy), white) end
    end
end

print("")
print("polygons (4 vertices, radius 6-12 px):   transform only | transform + draw")
local res_poly = {}
for _, P in ipairs({ 100, 200, 400 }) do
    local px, py, ang, spin, rad = make_polys(P, 4)
    local t_tr = bench(function() poly_frame(P, 4, px, py, ang, spin, rad, false) end)
    local t_dr = bench(function() poly_frame(P, 4, px, py, ang, spin, rad, true) end)
    res_poly[P] = t_dr
    print(string.format("  P=%3d                                  %s | %s", P, fmt(t_tr), fmt(t_dr)))
end

-- 4) physics วงกลมชนกัน: O(n^2) เทียบกับ grid -----------------------------------
local function make_bodies(n)
    local b = { x = {}, y = {}, vx = {}, vy = {}, n = n, r = 4 }
    for i = 1, n do
        b.x[i] = math.random(10, W - 10); b.y[i] = math.random(10, H - 10)
        b.vx[i] = (math.random() - 0.5) * 2; b.vy[i] = (math.random() - 0.5) * 2
    end
    return b
end

local function resolve(b, i, j)
    local x, y, vx, vy = b.x, b.y, b.vx, b.vy
    local dx, dy = x[j] - x[i], y[j] - y[i]
    local d2 = dx * dx + dy * dy
    local rr = b.r * 2
    if d2 < rr * rr and d2 > 0.0001 then
        local d = sqrt(d2)
        local nx, ny = dx / d, dy / d
        local push_ = (rr - d) * 0.5
        x[i] = x[i] - nx * push_; y[i] = y[i] - ny * push_
        x[j] = x[j] + nx * push_; y[j] = y[j] + ny * push_
        local rel = (vx[j] - vx[i]) * nx + (vy[j] - vy[i]) * ny
        if rel < 0 then   -- กำลังเข้าหากัน: สลับความเร็วตามแนวชน (มวลเท่ากัน)
            vx[i] = vx[i] + rel * nx; vy[i] = vy[i] + rel * ny
            vx[j] = vx[j] - rel * nx; vy[j] = vy[j] - rel * ny
        end
    end
end

local function integrate(b)
    local x, y, vx, vy = b.x, b.y, b.vx, b.vy
    for i = 1, b.n do
        local nx, ny = x[i] + vx[i], y[i] + vy[i]
        if nx < 4 or nx > W - 4 then vx[i] = -vx[i]; nx = x[i] end
        if ny < 4 or ny > H - 4 then vy[i] = -vy[i]; ny = y[i] end
        x[i], y[i] = nx, ny
    end
end

local function step_naive(b)
    integrate(b)
    local n = b.n
    for i = 1, n - 1 do
        for j = i + 1, n do resolve(b, i, j) end
    end
end

local CS = 16
local GW, GH = floor(W / CS) + 1, floor(H / CS) + 1
local head, nxt = {}, {}
local function step_grid(b)
    integrate(b)
    local n, x, y = b.n, b.x, b.y
    for c = 1, GW * GH do head[c] = 0 end
    for i = 1, n do
        local c = floor(x[i] / CS) + floor(y[i] / CS) * GW + 1
        nxt[i] = head[c]; head[c] = i
    end
    for i = 1, n do
        local cx, cy = floor(x[i] / CS), floor(y[i] / CS)
        for gy = cy - 1, cy + 1 do
            if gy >= 0 and gy < GH then
                for gx = cx - 1, cx + 1 do
                    if gx >= 0 and gx < GW then
                        local j = head[gx + gy * GW + 1]
                        while j ~= 0 do
                            if j > i then resolve(b, i, j) end
                            j = nxt[j]
                        end
                    end
                end
            end
        end
    end
end

print("")
print("physics (circles r=4): O(n^2) pairs | uniform grid   [ms per step]")
local res_phys = {}
for _, n in ipairs({ 50, 100, 200, 400 }) do
    local b1, b2 = make_bodies(n), make_bodies(n)
    local t_n = bench(function() step_naive(b1) end)
    local t_g = bench(function() step_grid(b2) end)
    res_phys[n] = t_g
    print(string.format("  n=%3d                  %s | %s", n, fmt(t_n), fmt(t_g)))
end

-- 5) สรุป: ประมาณเวลาเฟรมรวม (clear + วาด poly + physics แบบ grid) ----------------
print("")
print("estimated frame = clear + polygons(draw) + physics(grid)  (ยังไม่รวมเวลา push จอ)")
for _, n in ipairs({ 100, 200, 400 }) do
    local total = t_clear + res_poly[n] + res_phys[n]
    print(string.format("  %3d polygons + %3d bodies: %s  ->  %.0f fps (ถ้าไม่ติดเพดาน push จอ)", n, n, fmt(total), 1000 / total))
end

print("")
print("memory.free at end:", memory.free())
print("=== done ===")