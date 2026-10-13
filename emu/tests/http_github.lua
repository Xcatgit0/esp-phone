-- Live HTTPS test (needs internet): ./build/phone_emu --headless --run tests/http_github.lua
wifi.mode("sta"); wifi.start(); wifi.sta.config({ ssid = "emu", password = "" }); wifi.sta.connect()
local t0 = time.millis(); repeat delay(50) until wifi.sta.status().ip or time.millis() - t0 > 3000

local function show(label, r, e)
  if not r then print(label, "ERROR", e); return end
  print(string.format("%-22s status=%d ok=%s redirects=%d body=%d bytes  url=%s", label, r.status, tostring(r.ok), r.redirects, #r.body, r.url))
end

local r, e = wifi.http.get("https://raw.githubusercontent.com/torvalds/linux/master/README")
show("raw file", r, e)
if r then print("  first line:", (r.body:match("[^\n]*"))); print("  content-type:", r.headers["content-type"]) end

r, e = wifi.http.get("https://github.com/torvalds/linux/raw/master/README")      -- github.com -> 302 -> raw host
show("raw via redirect", r, e)

r, e = wifi.http.get("https://api.github.com/repos/torvalds/linux", { headers = { Accept = "application/vnd.github+json" } })
show("api.github.com", r, e)
if r and r.ok then print("  json starts:", r.body:sub(1, 60):gsub("%s+", " ")) end
if r then print("  x-ratelimit-remaining:", r.headers["x-ratelimit-remaining"]) end

r, e = wifi.http.get("https://raw.githubusercontent.com/torvalds/linux/master/does-not-exist")
show("missing file", r, e)

r, e = wifi.http.get("https://raw.githubusercontent.com/torvalds/linux/master/README", { max_body = 1000 })
show("max_body=1000", r, e)

r, e = wifi.http.get("https://expired.badssl.com/") ; show("bad certificate", r, e)
r, e = wifi.http.get("http://raw.githubusercontent.com/torvalds/linux/master/README", {redirects = 3}); show("http -> https upgrade", r, e)
