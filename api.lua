---@meta

---@class TimeAPI
time = {}

---Get system uptime in milliseconds.
---@return integer milliseconds
function time.millis() end

---@class MemoryAPI
memory = {}

---Get currently free heap memory.
---@return integer bytes
function memory.free() end

---Get currently used heap memory.
---@return integer bytes
function memory.used() end

---Delay execution for the specified number of milliseconds.
---@param ms integer
function delay(ms) end

---Dump All Task to Serial stdout(Serial)
function dumptask() end
---Read the entire contents of a file.
---@param path string
---@return string|nil data
function readfile(path) end

---@class CursorAPI
cursor = {}

---Get the current cursor position.
---@return integer x
---@return integer y
function cursor.pos() end

---Check whether the joystick/select button (GPIO46) is currently pressed.
---@return integer level 0 = pressed (pulled low), 1 = released
function cursor.isDown() end

---@class Button
button = {}

---Initializing button gpio(s)
function button.init() end

---Get Buttons state WARNING if calls this function before button.init this will returns {0,0,0}
---@return integer[] states
function button.get() end

---@class GfxAPI
gfx = {}

---Draw a line.
---@param x1 integer Starting X coordinate.
---@param y1 integer Starting Y coordinate.
---@param x2 integer Ending X coordinate.
---@param y2 integer Ending Y coordinate.
---@param color integer RGB565 color.
function gfx.line(x1, y1, x2, y2, color) end

---Draw a rectangle.
---@param x integer X coordinate.
---@param y integer Y coordinate.
---@param w integer Width.
---@param h integer Height.
---@param color integer RGB565 color.
---@param filled boolean Whether the rectangle is filled.
function gfx.rect(x, y, w, h, color, filled) end

---Draw a circle.
---@param cx integer Center X coordinate.
---@param cy integer Center Y coordinate.
---@param r integer Radius.
---@param color integer RGB565 color.
---@param filled boolean Whether the circle is filled.
function gfx.circle(cx, cy, r, color, filled) end

---Load a font and make it the default graphics font.
---@param path string Path to the font file.
---@return boolean success
function gfx.loadfont(path) end

---Draw text using the current default font.
---@param x integer X coordinate.
---@param y integer Y coordinate.
---@param text string Text to draw.
---@param fg integer Foreground RGB565 color.
---@param bg? integer Background RGB565 color. Defaults to 0x0000.
function gfx.text(x, y, text, fg, bg) end

---Push the current graphics framebuffer to the display.
function gfx.push() end

---convert color
---@param rgb888 integer RGB888
---@return integer
function gfx.rgb(rgb888) end
--============================================================
-- Wi-Fi / network API
--
-- All wifi.* calls are thin bindings straight into C (esp_wifi /
-- esp_netif / lwIP) -- none of them run a Lua-side polling loop.
-- wifi.sta.connect()/disconnect() are asynchronous (fire-and-forget);
-- everything else either returns immediately with cached state or
-- blocks briefly in C (e.g. wifi.scan()).
--============================================================

---@alias WifiMode
---| '"off"'
---| '"sta"'
---| '"ap"'
---| '"apsta"'

---@alias WifiIfaceName
---| '"sta"'
---| '"ap"'

---@class WifiStaInfo
---@field connected boolean
---@field ssid string
---@field bssid string BSSID formatted as "AA:BB:CC:DD:EE:FF".
---@field channel integer
---@field rssi integer dBm.
---@field auth string e.g. "OPEN", "WPA2_PSK", "WPA3_PSK".
---@field ip? string Only present once an IP has been obtained.
---@field gateway? string
---@field netmask? string

---@class WifiApInfo
---@field started boolean
---@field ssid string
---@field channel integer
---@field ip? string Only present while the AP is started.
---@field netmask? string
---@field clients integer Number of currently associated stations.

---@class WifiStatus
---@field mode WifiMode
---@field started boolean
---@field sta WifiStaInfo
---@field ap WifiApInfo

---@class WifiScanResult
---@field ssid string
---@field bssid string
---@field rssi integer dBm.
---@field channel integer
---@field auth string

---@class WifiApClient
---@field mac string
---@field ip? string Present only if resolvable via the DHCP server's MAC/IP table.
---@field rssi integer

---@class WifiDhcpLease
---@field mac string
---@field ip string

---@class WifiDhcpStatus
---@field running boolean

---@class WifiDnsStatus
---@field running boolean

---@class WifiNetInterface
---@field name WifiIfaceName
---@field up boolean
---@field ip? string
---@field netmask? string
---@field gateway? string

---@class WifiNetRequestResult
---@field success boolean
---@field local_ip? string The local IP the socket was bound to, if an interface was given.
---@field error? string Present only when success is false.

---@class WifiSignalInfo
---@field rssi? integer dBm. Absent if not connected.
---@field quality? integer Normalized 0-100 (own linear mapping, not an ESP-IDF-reported value). Absent if not connected.

---@class WifiStaConfig
---@field ssid string
---@field password? string Empty/omitted = open network. Otherwise must be 8-63 characters.

---@class WifiApConfig
---@field ssid string
---@field password? string Empty/omitted = open network. Otherwise must be 8-63 characters.
---@field channel? integer 1-13. Defaults to 1.
---@field max_connections? integer 1-10. Defaults to 4.
---@field hidden? boolean Defaults to false.

---@class WifiNetRequestOptions
---@field interface? WifiIfaceName Bind the outgoing socket's source IP to this interface before connecting.
---@field host string Hostname or IPv4 literal.
---@field port? integer Defaults to 80.
---@field timeout? integer Milliseconds. Defaults to 5000.

---@class WifiAPI
wifi = {}

---Get or set the Wi-Fi mode.
---@param mode? WifiMode Omit to just read the current mode.
---@return WifiMode current_mode
function wifi.mode(mode) end

---Start Wi-Fi with the previously configured mode/STA/AP settings.
---@return boolean ok
---@return string? err Present only on failure.
function wifi.start() end

---Stop Wi-Fi (disconnects STA, stops AP/DHCP server, tears down netifs).
---@return boolean ok
function wifi.stop() end

---Get a full snapshot of the current Wi-Fi status.
---@return WifiStatus status
function wifi.status() end

---Perform a blocking Wi-Fi scan. Requires wifi.start() to have been
---called first. Blocks the calling task for the duration of the scan
---(no Lua-side polling loop is involved).
---@return WifiScanResult[] networks
function wifi.scan() end

---@class WifiStaAPI
wifi.sta = {}

---Configure the STA (client) SSID/password. Takes effect on the next
---wifi.start(), or immediately if Wi-Fi is already running in a
---STA-capable mode.
---@param config WifiStaConfig
---@return boolean ok
function wifi.sta.config(config) end

---Asynchronously connect to the configured STA network. Returns
---immediately; use wifi.sta.status()/wifi.status() to observe the
---result once WIFI_EVENT_STA_CONNECTED/DISCONNECTED fires internally.
---@return boolean ok
function wifi.sta.connect() end

---Disconnect the STA link.
---@return boolean ok
function wifi.sta.disconnect() end

---@return WifiStaInfo info
function wifi.sta.status() end

---Alias of wifi.sta.status().
---@return WifiStaInfo info
function wifi.sta.info() end

---@return integer|nil rssi dBm, or nil if not connected.
function wifi.sta.rssi() end

---@return string|nil ip nil if no IP has been obtained yet.
function wifi.sta.ip() end

---@return string|nil gateway nil if no IP has been obtained yet.
function wifi.sta.gateway() end

---@return string|nil dns nil if unavailable.
function wifi.sta.dns() end

---@class WifiApAPI
wifi.ap = {}

---Configure the SoftAP. Takes effect on the next wifi.start(), or
---immediately if Wi-Fi is already running in an AP-capable mode.
---@param config WifiApConfig
---@return boolean ok
function wifi.ap.config(config) end

---(Re)start the AP (starts Wi-Fi entirely if it wasn't running yet).
---@return boolean ok
function wifi.ap.start() end

---Stop the AP (drops "apsta" to "sta", or fully stops Wi-Fi if mode was "ap").
---@return boolean ok
function wifi.ap.stop() end

---@return WifiApInfo info
function wifi.ap.status() end

---Alias of wifi.ap.status().
---@return WifiApInfo info
function wifi.ap.info() end

---List stations currently associated with the SoftAP.
---@return WifiApClient[] clients
function wifi.ap.clients() end

---@return string|nil ip nil if the AP isn't started.
function wifi.ap.ip() end

---Whether the DHCP server is currently running on the AP interface.
---@return boolean running
function wifi.ap.dhcp() end

---@class WifiDhcpAPI
wifi.dhcp = {}

---Configure the DHCP server's address pool (and optionally the AP's
---own static IP/netmask/gateway).
---
---**IMPORTANT:** `end` is a reserved word in Lua and cannot be written
---as `end = "192.168.4.100"` inside a table constructor (that is a
---Lua syntax error). Use `["end"] = "192.168.4.100"` instead.
---
---Fields: `ip?`, `netmask?`, `gateway?` (AP static IP, optional),
---`start` (string, required), `["end"]` (string, required),
---`lease_time?` (integer seconds).
---@param config table
---@return boolean ok
function wifi.dhcp.config(config) end

---@return boolean ok
function wifi.dhcp.start() end

---@return boolean ok
function wifi.dhcp.stop() end

---@return WifiDhcpStatus status
function wifi.dhcp.status() end

---@return WifiDhcpLease[] leases
function wifi.dhcp.leases() end

---@class WifiDnsAPI
wifi.dns = {}

---Start the local DNS server (serves local/hash records, forwards
---everything else to the configured upstream servers).
---@return boolean ok
function wifi.dns.start() end

---@return boolean ok
function wifi.dns.stop() end

---@return WifiDnsStatus status
function wifi.dns.status() end

---Get the currently configured upstream DNS servers.
---@return string[] servers
---@overload fun(servers: string[]): boolean
function wifi.dns.upstream() end

---Add/overwrite a local hostname -> IP record (e.g. for ad-blocking:
---set ip to "0.0.0.0" to sinkhole).
---@param hostname string
---@param ip string
---@return boolean ok
function wifi.dns.add(hostname, ip) end

---@param hostname string
---@return boolean ok
function wifi.dns.remove(hostname) end

---Clear all local hostname and hash records.
function wifi.dns.clear() end

---@param hostname string
---@return string|nil ip nil if no local record matches.
function wifi.dns.lookup(hostname) end

---Add/overwrite a local record keyed by a pre-computed numeric hash
---instead of a hostname string (saves RAM for large block-lists). If
---the table has never been sized with wifi.dns.hash_init(), the first
---call lazily creates a small default-sized table.
---@param hash integer
---@param ip string
---@return boolean ok
---@return string? err esp_err name if ok is false (e.g. table full).
function wifi.dns.add_hash(hash, ip) end

---@param hash integer
---@return string|nil ip nil if no local record matches.
function wifi.dns.hash_lookup(hash) end

---(Re)allocate the hash-record table for at least `capacity` entries.
---Call this up front with your real expected block-list size --
---inserts past ~70% load factor of whatever size is currently
---allocated will start failing with ESP_ERR_NO_MEM. **Dropping any
---existing hash entries** -- call before populating the list, not
---mid-use. Storage cost is ~8.25 bytes/entry (real ESP-IDF/lwIP heap,
---auto-placed in PSRAM by the allocator once the table exceeds 16KB).
---@param capacity integer
---@return boolean ok
function wifi.dns.hash_init(capacity) end

---@return integer count Number of live hash entries currently stored.
function wifi.dns.hash_count() end

---@return integer capacity Current allocated table size (entries).
function wifi.dns.hash_capacity() end

---@class WifiNetAPI
wifi.net = {}

---@return WifiNetInterface[] interfaces
function wifi.net.interfaces() end

---@param name WifiIfaceName
---@return WifiNetInterface|nil info nil if that interface isn't up.
function wifi.net.info(name) end

---@param name WifiIfaceName
---@return string|nil ip
function wifi.net.ip(name) end

---@param name WifiIfaceName
---@return string|nil gateway
function wifi.net.gateway(name) end

---@param name WifiIfaceName
---@return string|nil dns
function wifi.net.dns(name) end

---Get the name of the interface currently used as lwIP's default route.
---@return WifiIfaceName|'"none"' iface
function wifi.net.route() end

---Get (no args) or set which interface lwIP treats as the default
---route (esp_netif_set_default_netif). This is NOT a per-socket
---selector -- see wifi.net.request() for that.
---@param name? WifiIfaceName
---@return WifiIfaceName|boolean result Current default name (get form) or success (set form).
function wifi.net.interface(name) end

---Best-effort interface-bound TCP connect probe (binds the socket's
---source IP to the chosen interface before connecting). Not a general
---HTTP client -- just tests reachability and reports the outcome.
---@param options WifiNetRequestOptions
---@return WifiNetRequestResult result
function wifi.net.request(options) end

---Turn on/off internet sharing: NAT (NAPT) from SoftAP clients out
---through the STA link, like a Wi-Fi repeater/router. Requires mode
---"apsta" with STA already connected and holding an IP -- call this
---after that, not right after wifi.start(). Requires
---CONFIG_LWIP_IP_FORWARD + CONFIG_LWIP_IPV4_NAPT in sdkconfig.
---@param enable? boolean Defaults to true.
---@return boolean ok
---@return string? err esp_err name if ok is false.
function wifi.net.share(enable) end

---@return boolean sharing Whether internet sharing is currently enabled.
function wifi.net.sharing() end

---@class WifiSignalAPI
wifi.signal = {}
---Set Tx Power
---@param power integer
function wifi.signal.setPower(power) end

---Alias of wifi.sta.rssi().
---@return integer|nil rssi
function wifi.signal.rssi() end

---@return integer|nil quality Normalized 0-100, nil if not connected.
function wifi.signal.quality() end

---@return WifiSignalInfo info
function wifi.signal.info() end