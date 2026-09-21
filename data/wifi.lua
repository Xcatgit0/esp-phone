-- ตัวอย่างทดสอบ wifi API (วางแทน/รวมกับ bootstrap.lua เดิม)
dumptask()
print("mode:", wifi.mode())
wifi.sta.config({ ssid = "Labunma", password = "01082502" })

wifi.ap.config({
    ssid = "ESP32-Gateway",
    password = "12345678",
    channel = 1,
    max_connections = 8,
    hidden = false
})

wifi.mode("apsta")
wifi.start()
wifi.signal.setPower(80)
delay(5000) -- ให้เวลา STA เชื่อมต่อ (connect เป็น async)

print("sta status:")
local sta = wifi.sta.status()
for k, v in pairs(sta) do print(" ", k, v) end

print("sta rssi:", wifi.sta.rssi())

print("ap status:")
local ap = wifi.ap.status()
for k, v in pairs(ap) do print(" ", k, v) end

local ok, err = wifi.net.share(true)   -- เปิด NAT: client บน AP -> ออกทาง STA
if not ok then print("share failed:", err) end

print(wifi.net.sharing())              -- true
-- สำคัญ: "end" เป็นคำสงวนของ Lua ใช้ ["end"] เท่านั้น ห้ามเขียน end = "..."
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

print("scanning...")
local networks = wifi.scan()
for i, n in ipairs(networks) do
    print(string.format("  %s  ch=%d  rssi=%d  %s", n.ssid, n.channel, n.rssi, n.auth))
end

print("interfaces:")
local ifs = wifi.net.interfaces()
for i, n in ipairs(ifs) do
    print(" ", n.name, n.up, n.ip)
end

print("default route:", wifi.net.route())
