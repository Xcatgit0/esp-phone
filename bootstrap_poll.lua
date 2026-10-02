print("TEST");
for k,v in pairs(_G) do
    print(k,v)
end
gfx.loadfont("/fs/font.psf")
print(memory.used())
gfx.text(10,5,"Hello World!",0xFFFF)
gfx.push()
local W = 320
local H = 240

local colors = {
    0xF800, -- Red
    0x07E0, -- Green
    0x001F, -- Blue
    0xFFE0, -- Yellow
    0x07FF, -- Cyan
    0xF81F, -- Magenta
    0xFFFF, -- White
    0x0000, -- Black
}

local barH = math.floor(H / #colors)

for i, color in ipairs(colors) do
    local y = (i - 1) * barH

    gfx.rect(
        0,
        y,
        W,
        barH,
        color,
        true
    )
end

gfx.push()
delay(500)
gfx.rect(0,0,319,239,0x0000,true)
gfx.push()
delay(250)
print("init buttons")
button.init()
gfx.rect(0,0,319,239,0x0000,true)
print("starting wifi")
wifi.mode("apsta")
wifi.ap.config({
    ssid = "ESP32-Gateway",
    password = "12345678",
    channel = 1,
    max_connections = 4,
    hidden = false
})

wifi.start()
wifi.sta.disconnect()
local networks = wifi.scan()
local cols = 16
local maxLines = 15

local selected = 1
local offset = 1

local password = ""
local ssid = ""
local entered = false

assert(cursor.poll, "cursor.poll missing: add cursor_poll_register(L) to API_INIT in api.c")

-- id ของ cursor.poll: 0/1 = ปุ่มกด joystick, 2 = KEY1, 3 = KEY2, 4 = KEY3
local KEY_UP, KEY_DOWN, KEY_ENTER = 2, 3, 4
local REPEAT_FIRST = 400   -- ms ที่ต้องกดค้างก่อนเริ่มเลื่อนซ้ำ
local REPEAT_RATE  = 100   -- ms ระหว่างการเลื่อนซ้ำ ขณะกดค้าง

local function draw_list()
    gfx.rect(0,0,319,239,0x0000,true)

    -- ทำให้ selected อยู่ในช่วงที่แสดง
    if selected < offset then
        offset = selected
    elseif selected >= offset + maxLines then
        offset = selected - maxLines + 1
    end

    -- วาดเฉพาะรายการที่อยู่ในหน้าจอ
    for i = 1, maxLines do
        local k = offset + i - 1
        local v = networks[k]

        if v then
            local text = string.format("%-30s %d dBm", v.ssid, v.rssi)

            gfx.text(
                0,
                (i - 1) * cols,
                text,
                (selected == k) and gfx.rgb(0x3333ff) or 0xffff
            )
        end
    end

    gfx.push()
end

local function move(id)
    if id == KEY_UP then
        selected = math.max(1, selected - 1)
    elseif id == KEY_DOWN then
        selected = math.min(#networks, selected + 1)
    end
end

-- หลับรอจนมีการ "กด" ปุ่มใดปุ่มหนึ่ง (การปล่อยปุ่มไม่นับ) ไม่ busy loop
local function wait_press()
    while true do
        local id, pressed = cursor.poll({ KEY_UP, KEY_DOWN, KEY_ENTER })
        if pressed then return id end
    end
end

draw_list()

repeat
    local id = wait_press()

    if id == KEY_ENTER then
        entered = true
    else
        move(id)
        draw_list()

        -- กดค้าง = เลื่อนซ้ำ
        -- ปุ่มนี้ถูกกดอยู่ตอนเรียก poll จึงเป็น baseline: event ที่ได้คือการปล่อย
        -- ถ้า timeout (nil) แปลว่ายังกดอยู่ -> เลื่อนอีกหนึ่งที (button.get ใช้ยืนยันว่ายังกดจริง)
        local wait = REPEAT_FIRST
        while cursor.poll(id, wait) == nil and button.get()[id - 1] == 1 do
            move(id)
            draw_list()
            wait = REPEAT_RATE
        end
    end
until entered
gfx.rect(0,0,319,239,0x0000,true)
gfx.text(20,20,"Finding Password in password.lua",0xffff)
gfx.push()
ssid = networks[selected].ssid
local passwords = loadfile("/fs/password.lua")()
local password = passwords[ssid]
if not password then
    gfx.text(20,20,"Could not find password exitting",0xffff)
    gfx.push()
    return
else
    gfx.text(20,20,"Password found!",0xffff)
    gfx.text(20,27,"Ssid: "..ssid.." Passwd:",0xffff)
    gfx.text(20,35,password,0xffff)
    gfx.text(20,42,"Starting Wifi and AP",0xffff)
    gfx.push()
end
print("Connecting",ssid,password)
wifi.sta.config({ ssid = ssid, password = password })
wifi.sta.connect()
delay(500)
gfx.rect(0,0,319,239,0x0000,true)
local function pl(l,text,fg,bg) 
    gfx.text(1,l*cols,text,fg or 0xffff,bg or 0x0000)
end
local ok = function(bool) 
    return (bool and "OK") or "NO"
end
pl(0,"Connecting...")
gfx.push()
repeat
    delay(125)
    print(".")
until wifi.sta.status().ip
local wok, err = wifi.net.share(true)   -- เปิด NAT: client บน AP -> ออกทาง STA
if not wok then print("share failed:", err) end
wifi.dhcp.config({
    ip = "192.168.4.1",
    netmask = "255.255.255.0",
    gateway = "192.168.4.1",
    start = "192.168.4.10",
    ["end"] = "192.168.4.100",
    lease_time = 3600
})
wifi.dhcp.start()

wifi.dns.upstream({ "8.8.8.8", "1.1.1.1", "9.9.9.9" })
wifi.dns.add("example.com", "0.0.0.0")
wifi.dns.start()

touch = loadfile("/fs/touch.lua")()
touch.recalibrate()
while true do
    delay(250)
    gfx.rect(0,0,319,239,0x0000,true)
    local status = wifi.status()
    pl(0,"SoftAP: "..ok(status.started).." Net Sharing: "..tostring(wifi.net.sharing()).." Route:"..wifi.net.route())
    pl(1,"STA: "..ok(status.sta.connected).." AP: "..ok(status.ap.started).. " UpTime: "..string.format("%.1f min %.1f secs",time.millis()/1000/60,(time.millis()/1000)%60))
    pl(2, "AP IP: "..status.ap.ip.."  ".."STA IP: "..status.sta.ip)
    pl(3, "RSSI "..status.sta.rssi.." dBm AP#"..status.ap.clients.." Ch "..status.ap.channel..":"..status.sta.channel)
    local devices = wifi.ap.clients()
    local startL = 4
    for k,v in ipairs(devices) do
        pl(startL-1+(k*2),string.format("#%1d %-17s %4d dBm MAC %s",k,v.ip,v.rssi,v.mac))
    end
    pl(14,string.format("X%.2f Y%.2f Z1 %.2f Z2 %.2f",cursor.touch()))
    local x,y = touch.read()
    if not x then x = 0 end
    if not y then y = 0 end
    gfx.circle(x,y,10,0xFFFF,true)
    --print (cursor.touch())
    gfx.push()
end