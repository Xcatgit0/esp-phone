-- wifi.http tests against tests/http_server.py (started by run_tests.sh on 127.0.0.1:18080)
local B = "http://127.0.0.1:18080"
local fails = 0
local function check(name, cond, extra)
  if cond then print("PASS  " .. name)
  else fails = fails + 1; print("FAIL  " .. name .. (extra ~= nil and ("  (" .. tostring(extra) .. ")") or "")) end
end
local function has(s, sub) return type(s) == "string" and s:find(sub, 1, true) ~= nil end

check("API present", type(wifi.http) == "table" and type(wifi.http.request) == "function" and type(wifi.http.get) == "function"
      and type(wifi.http.post) == "function" and type(wifi.http.start) == "function"
      and type(wifi.http.poll) == "function" and type(wifi.http.cancel) == "function")

-- no network yet
local r, e = wifi.http.get(B .. "/hello")
check("no network -> nil, err", r == nil and has(e, "no network"), e)

-- bring the (mock) STA up
wifi.mode("sta"); wifi.start()
wifi.sta.config({ ssid = "emu", password = "" }); wifi.sta.connect()
local t0 = time.millis()
repeat delay(50) until wifi.sta.status().ip or time.millis() - t0 > 3000
check("sta has ip", wifi.sta.status().ip ~= nil)

-- basic GET
r, e = wifi.http.get(B .. "/hello")
check("GET 200", r and r.status == 200 and r.ok == true and r.body == "hello world", e)
check("headers lowercase + joined", r and r.headers["x-test"] == "abc" and r.headers["x-multi"] == "a, b"
      and r.headers["content-type"] == "text/plain", r and r.headers["x-multi"])
check("final url / redirects", r and r.url == B .. "/hello" and r.redirects == 0, r and r.url)

-- HEAD
r, e = wifi.http.request{ url = B .. "/hello", method = "head" }
check("HEAD: no body, content-length header", r and r.status == 200 and r.body == "" and r.headers["content-length"] == "11", e)

-- default and custom headers
r = wifi.http.get(B .. "/headers")
check("default User-Agent", r and has(r.body, "ESP32-Phone/1.0"), r and r.body)
r = wifi.http.get(B .. "/headers", { headers = { ["User-Agent"] = "mygame/2", Authorization = "Bearer t0k3n", ["X-N"] = 5 } })
check("custom headers (UA override, number value)", r and has(r.body, "mygame/2") and has(r.body, "Bearer t0k3n") and has(r.body, "X-N: 5")
      and not has(r.body, "ESP32-Phone"), r and r.body)

-- methods + bodies
r = wifi.http.post(B .. "/echo", "ping", { headers = { ["X-Custom"] = "yes", ["Content-Type"] = "application/json" } })
check("POST body + headers", r and has(r.body, "method=POST") and has(r.body, "body=ping") and has(r.body, "x-custom=yes")
      and has(r.body, "content-type=application/json"), r and r.body)
for _, m in ipairs({ "PUT", "PATCH", "DELETE" }) do
  r = wifi.http.request{ url = B .. "/echo", method = m, body = "b-" .. m }
  check(m .. " with body", r and has(r.body, "method=" .. m) and has(r.body, "body=b-" .. m), r and r.body)
end
r = wifi.http.request{ url = B .. "/echo", method = "POST", body = "a\0b\r\n\255" }
check("binary-safe request body", r and has(r.body, "body=a") , r and #r.body)

-- statuses are results, not errors
r, e = wifi.http.get(B .. "/status/404")
check("404 is a normal result", r and r.status == 404 and r.ok == false and r.body == "status 404", e)
r = wifi.http.get(B .. "/status/500")
check("500 is a normal result", r and r.status == 500 and r.ok == false)
r = wifi.http.get(B .. "/empty")
check("204 empty body", r and r.status == 204 and r.body == "")

-- redirects
r, e = wifi.http.get(B .. "/redir2")
check("follows 301 -> 302 chain", r and r.status == 200 and r.body == "hello world" and r.redirects == 2 and r.url == B .. "/hello", e)
r = wifi.http.get(B .. "/redir-rel")
check("relative Location", r and r.status == 200 and r.url == B .. "/hello", r and r.url)
r = wifi.http.get(B .. "/redir", { redirects = 0 })
check("redirects=0 returns the 302", r and r.status == 302 and r.headers.location == "/hello", r and r.status)
r, e = wifi.http.get(B .. "/loop")
check("redirect loop -> error", r == nil and has(e, "too many redirects"), e)
r, e = wifi.http.get(B .. "/loop", { redirects = 2 })
check("redirect limit honoured", r == nil and has(e, "too many redirects (2)"), e)
r = wifi.http.post(B .. "/redir-post", "data")
check("POST + 302 -> GET without body", r and has(r.body, "method=GET") and has(r.body, "body=\n"), r and r.body)
r = wifi.http.post(B .. "/redir-307", "data")
check("POST + 307 keeps method and body", r and has(r.body, "method=POST") and has(r.body, "body=data"), r and r.body)

-- sizes
r, e = wifi.http.get(B .. "/big?n=200000")
check("Content-Length > max_body rejected up front", r == nil and has(e, "too large"), e)
r, e = wifi.http.get(B .. "/big?n=200000", { max_body = 300000 })
check("bigger max_body accepts it", r and #r.body == 200000, e)
r = wifi.http.get(B .. "/chunked")
check("chunked transfer", r and r.body == "chunk1chunk2chunk3" and r.status == 200, r and r.body)
r, e = wifi.http.get(B .. "/chunked-big")
check("chunked body over max_body rejected", r == nil and has(e, "max_body"), e)

-- transport errors
local t1 = time.millis()
r, e = wifi.http.get(B .. "/slow", { timeout = 500 })
check("timeout -> nil, 'timed out'", r == nil and has(e, "timed out"), e)
check("...after about 500 ms", time.millis() - t1 < 1500, time.millis() - t1)
r, e = wifi.http.get("http://127.0.0.1:1/")
check("connection refused", r == nil and has(e, "cannot connect"), e)

-- option validation (returned as nil, err; programmer errors raise)
r, e = wifi.http.get("ftp://x/y");                                  check("bad scheme", r == nil and has(e, "http://"), e)
r, e = wifi.http.get("http://a b/");                                check("space in url", r == nil and has(e, "spaces"), e)
r, e = wifi.http.get(B .. "/hello", { headers = { ["X-A"] = "x\r\nInjected: 1" } })
check("header injection refused", r == nil and has(e, "CR, LF"), e)
r, e = wifi.http.get(B .. "/hello", { headers = { Host = "evil" } })
check("managed header refused", r == nil and has(e, "set automatically"), e)
r, e = wifi.http.request{ url = B .. "/hello", method = "GET", body = "x" }
check("GET with body refused", r == nil and has(e, "cannot have a body"), e)
r, e = wifi.http.request{ url = B .. "/hello", method = "FOO" }
check("unknown method", r == nil and has(e, "unsupported method"), e)
r, e = wifi.http.request{ url = B .. "/hello", ca_cert = "nope" }
check("ca_cert must be PEM", r == nil and has(e, "PEM"), e)
r, e = wifi.http.request{ url = B .. "/hello", timeout = "fast" }
check("timeout type checked", r == nil and has(e, "timeout"), e)
check("non-table argument raises", not pcall(wifi.http.request, "http://x"))
check("get without url raises", not pcall(wifi.http.get))

-- async
local id = wifi.http.start{ url = B .. "/slow", timeout = 5000 }
check("start returns an id", math.type(id) == "integer", id)
check("poll(id) is non-blocking while running", wifi.http.poll(id) == false)
local spins, ta = 0, time.millis()
while wifi.http.poll(id) == false and time.millis() - ta < 5000 do spins = spins + 1; delay(50) end   -- Lua keeps running
check("Lua loop kept running during the request", spins >= 10, spins)
-- the result was consumed by the loop's last poll; start another to test blocking poll + cpu
id = wifi.http.start{ url = B .. "/slow", timeout = 5000 }
local c0, w0 = emu.cpu_ms(), time.millis()
r, e = wifi.http.poll(id, 5000)
local cpu, wall = emu.cpu_ms() - c0, time.millis() - w0
check("poll(id, timeout) blocks until done", r and r.body == "slow", e)
check("...without busy looping", cpu < wall * 0.3, string.format("cpu %.0f ms / wall %d ms", cpu, wall))
check("collected id is gone", not pcall(wifi.http.poll, id))

id = wifi.http.start{ url = B .. "/slow" }
r = wifi.http.poll(id, 100)
check("poll(id, 100) times out with false", r == false)
check("cancel returns true", wifi.http.cancel(id) == true)
check("cancel twice returns false", wifi.http.cancel(id) == false)
check("cancelled id is gone", not pcall(wifi.http.poll, id))
delay(2500)   -- let the worker finish the abandoned request

-- slot limit
local ids = {}
for i = 1, 4 do ids[i] = wifi.http.start{ url = B .. "/slow" } end
r, e = wifi.http.start{ url = B .. "/hello" }
check("5th outstanding request refused", r == nil and has(e, "too many outstanding"), e)
for i = 1, 4 do wifi.http.cancel(ids[i]) end
delay(9000)   -- 4 x 2 s of server sleep, serialized on the worker (cancelled ones are skipped/aborted)
r, e = wifi.http.get(B .. "/hello")
check("slots are free again", r and r.status == 200, e)

print(fails == 0 and "ALL PASSED" or (fails .. " FAILED"))
emu.quit()
