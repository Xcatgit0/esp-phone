print("TEST");
for k,v in pairs(_G) do
    print(k,v)
end
gfx.loadfont("/fs/font.psf")
print(memory.used())
gfx.text(10,5,"Hello World!",0xFFFF)
gfx.push()
local colors = {
        0x0000, -- Black
        0xF800, -- Red
        0x07E0, -- Green
        0x001F, -- Blue
        0xFFE0, -- Yellow
        0x07FF, -- Cyan
        0xF81F, -- Magenta
        0xFFFF, -- White
        0x8410, -- Gray
        0x8000, -- Dark Red
        0x0400, -- Dark Green
        0x0010, -- Dark Blue
        0x8400, -- Olive
        0x0410, -- Dark Cyan
        0x8010, -- Dark Purple
        0xC618  -- Light Gray
    }

    local size = 20

    for i, color in ipairs(colors) do
        local x = ((i - 1) % 4) * size
        local y = 25 + math.floor((i - 1) / 4) * size

        gfx.rect(x, y, size, size, color, true)
       --print(i)
    end
    print(memory.used())
gfx.push()
print("init buttons")
button.init()
gfx.rect(0,0,169,319,0x0000,true)
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
local maxLines = 10

local selected = 1
local offset = 1

local password = ""
local ssid = ""
local entered = false

repeat
    gfx.rect(0,0,319,169,0x0000,true)

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

    local changed = false

    repeat
        delay(250)

        local button = button.get()

        if button[3] == 1 then
            entered = true
        end

        if button[2] == 1 then
            selected = selected + 1
            if selected > #networks then
                selected = #networks
            end
        end

        if button[1] == 1 then
            selected = selected - 1
            if selected < 1 then
                selected = 1
            end
        end

        if button[1] == 0 or button[2] == 0 then
            changed = true
        end
    until changed

until entered
gfx.rect(0,0,319,169,0x0000,true)
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
gfx.rect(0,0,319,169,0x0000,true)
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

while true do
    delay(250)
    gfx.rect(0,0,319,169,0x0000,true)
    local status = wifi.status()
    pl(0,"SoftAP: "..ok(status.started).." Net Sharing: "..tostring(wifi.net.sharing()).." Route:"..wifi.net.route())
    pl(1,"STA: "..ok(status.sta.connected).." AP: "..ok(status.ap.started).. " UpTime: "..string.format("%.1f min %.1f secs",time.millis()/1000/60,(time.millis()/1000)%60))
    pl(2, "AP IP: "..status.ap.ip.."  ".."STA IP: "..status.sta.ip)
    pl(3, "RSSI "..status.sta.rssi.." dBm AP#"..status.ap.clients.." Ch "..status.ap.channel..":"..status.sta.channel)
    local devices = wifi.ap.clients()
    local startL = 4
    for k,v in ipairs(devices) do
        pl(startL-1+k,string.format("#%1d %-17s %4d dBm MAC %s",k,v.ip,v.rssi,v.mac))
    end
    gfx.push()
end

