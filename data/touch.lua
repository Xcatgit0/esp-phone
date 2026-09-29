-- touch.lua : calibrate ตอนโหลดจากข้อมูลที่ฝังไว้ (แก้ตัวเลขได้ตรง SAMPLES)

local W, H = 320, 240
local Z1_PRESS_MIN = 60   -- ถ้า z1 ต่ำกว่านี้ถือว่าไม่ได้กด (ปรับตามที่วัดตอนไม่แตะจอ)

-- แต่ละจุด: พิกัดจอ sx, sy และ samples = { {x, y, z1, z2}, ... }
local SAMPLES = {
  { sx = 0, sy = 0, name = "top_left", samples = {
    {270,3768,258,3919},{264,3775,283,3904},{268,3768,286,3899},
    {278,3783,285,3904},{276,3791,289,3904},{276,3775,285,3904},
    {278,3768,286,3911},{277,3808,284,3903},{276,3775,285,3905},
  }},
  { sx = 320, sy = 0, name = "top_right", samples = {
    {3883,3818,2064,3967},{3883,3839,2056,4016},{3935,3724,2032,3964},
    {3891,3818,2064,3982},{3911,3840,2080,3992},{3872,3808,2060,4007},
    {3896,3822,2071,4032},{3864,3822,2071,4005},{3896,3822,2071,4023},
    {3886,3820,2071,4032},{3907,3818,2064,3967},{3852,3823,2035,4032},
    {3879,3840,2057,3992},
  }},
  { sx = 0, sy = 240, name = "bottom_left", samples = {
    {231,279,153,2542},{231,279,154,2545},{233,280,156,2561},
    {232,276,154,2543},{232,278,153,2550},{233,279,154,2527},
    {232,280,155,2563},{233,279,154,2526},{233,278,154,2540},
    {233,280,154,2568},
  }},
  { sx = 320, sy = 240, name = "bottom_right", samples = {
    {3800,262,1667,3099},{3852,286,1691,3104},{3804,263,1687,3088},
    {3877,282,1688,3086},{3872,280,1678,3095},{3855,262,1692,3056},
    {3872,282,1679,3095},{3806,262,1683,3088},{3904,280,1616,3080},
    {3735,273,1608,3120},
  }},
}

---------------------------------------------------------------- helpers

local function median(t)
  local s = {}
  for i = 1, #t do s[i] = t[i] end
  table.sort(s)
  local n = #s
  if n % 2 == 1 then return s[(n + 1) // 2] end
  return (s[n // 2] + s[n // 2 + 1]) / 2
end

local function det3(m)
  return m[1][1]*(m[2][2]*m[3][3] - m[2][3]*m[3][2])
       - m[1][2]*(m[2][1]*m[3][3] - m[2][3]*m[3][1])
       + m[1][3]*(m[2][1]*m[3][2] - m[2][2]*m[3][1])
end

local function solve3(A, B)
  local d = det3(A)
  if math.abs(d) < 1e-9 then return nil end
  local out = {}
  for col = 1, 3 do
    local M = {
      {A[1][1], A[1][2], A[1][3]},
      {A[2][1], A[2][2], A[2][3]},
      {A[3][1], A[3][2], A[3][3]},
    }
    for r = 1, 3 do M[r][col] = B[r] end
    out[col] = det3(M) / d
  end
  return out
end

-- points = { {rx, ry, sx, sy}, ... } อย่างน้อย 3 จุดที่ไม่อยู่ในเส้นตรงเดียวกัน
local function fit(points)
  local n = #points
  local sxx, sxy, sx, syy, sy = 0, 0, 0, 0, 0
  local bxx, bxy, bx1, byx, byy, by1 = 0, 0, 0, 0, 0, 0
  for _, p in ipairs(points) do
    sxx = sxx + p.rx * p.rx;  sxy = sxy + p.rx * p.ry
    sx  = sx  + p.rx;         syy = syy + p.ry * p.ry
    sy  = sy  + p.ry
    bxx = bxx + p.rx * p.sx;  bxy = bxy + p.ry * p.sx;  bx1 = bx1 + p.sx
    byx = byx + p.rx * p.sy;  byy = byy + p.ry * p.sy;  by1 = by1 + p.sy
  end
  local A = { {sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, n} }
  local px = solve3(A, {bxx, bxy, bx1})
  local py = solve3(A, {byx, byy, by1})
  if not px or not py then return nil end
  return { a = px[1], b = px[2], c = px[3],
           d = py[1], e = py[2], f = py[3] }
end

---------------------------------------------------------------- calibrate

local Touch = {}
local cal

-- เรียกใหม่ได้ถ้าแก้ SAMPLES ตอนรัน
function Touch.recalibrate()
  local pts = {}
  for _, p in ipairs(SAMPLES) do
    local xs, ys = {}, {}
    for i, s in ipairs(p.samples) do
      xs[i] = s[1]
      ys[i] = s[2]
    end
    pts[#pts + 1] = { rx = median(xs), ry = median(ys), sx = p.sx, sy = p.sy }
  end
  cal = fit(pts)
  if not cal then error("touch calibration failed: points are collinear") end

  -- ค่าคลาดเคลื่อนเฉลี่ย (พิกเซล) ไว้ดีบัก
  local err = 0
  for _, p in ipairs(pts) do
    local x = cal.a * p.rx + cal.b * p.ry + cal.c
    local y = cal.d * p.rx + cal.e * p.ry + cal.f
    err = err + math.sqrt((x - p.sx)^2 + (y - p.sy)^2)
  end
  Touch.avg_error = err / #pts
  return cal
end

---------------------------------------------------------------- use

-- แปลงค่าดิบเป็นพิกัดจอ (clamp ในช่วง 0..W-1, 0..H-1)
function Touch.map(rx, ry)
  local x = cal.a * rx + cal.b * ry + cal.c
  local y = cal.d * rx + cal.e * ry + cal.f
  x = math.max(0, math.min(W - 1, math.floor(x + 0.5)))
  y = math.max(0, math.min(H - 1, math.floor(y + 0.5)))
  return x, y
end
  local function spread(t)
    local lo, hi = t[1], t[1]
    for i = 2, #t do
      if t[i] < lo then lo = t[i] end
      if t[i] > hi then hi = t[i] end
    end
    return hi - lo
  end
local MAX_SPREAD = 100   -- ค่าดิบต่างกันเกินนี้ถือว่าไม่นิ่ง (ปรับตามที่วัดได้)
local MIN_VALID  = 3     -- ต้องมี sample ที่กดจริงอย่างน้อยกี่ตัว

-- คืน x, y (พิกัดจอ) หรือ nil ถ้าไม่ได้กด / ค่าไม่นิ่ง
function Touch.read(count)
  count = count or 6

  cursor.touch()                       -- ทิ้งการอ่านแรก รอให้แรงดันนิ่ง

  local xs, ys = {}, {}
  for i = 1, count do
    local x, y, z1 = cursor.touch()
    if z1 >= Z1_PRESS_MIN then         -- เก็บเฉพาะรอบที่กดอยู่จริง
      xs[#xs + 1] = x
      ys[#ys + 1] = y
    end
  end

  if #xs < MIN_VALID then return nil end

  -- เช็คว่าค่านิ่งพอ (ตัดจังหวะแตะ/ยกนิ้ว)

  if spread(xs) > MAX_SPREAD or spread(ys) > MAX_SPREAD then return nil end

  return Touch.map(median(xs), median(ys))
end

Touch.recalibrate()
return Touch