/*
 * emu_wifi.cpp - mock of src/wifi/wifi_core.c, dns_server.c and http_server.c so that the
 * project's own src/wifi/lua_wifi.c (the Lua `wifi` API) compiles and runs unmodified.
 *
 * Behaviour mirrors wifi_core.c (return codes, "off" until first set_mode, async connect, ...).
 * There is no radio: the "air" is a list of fake networks. Default: one open network "emu".
 * Add more from Lua with emu.wifi_add_network(...) or from C++ with internal::wifiAddNetwork().
 * wifi.net.request() performs a REAL TCP connect on the host once the interface is "up".
 */
#include "emu_internal.h"
#include <arpa/inet.h>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "wifi_core.h"
#include "dns_server.h"
#include "http_server.h"
#include "esp_wifi.h"
}

using Clock = std::chrono::steady_clock;

namespace {
struct Net { std::string ssid, auth, bssid, password; int rssi, channel; };
struct State {
    bool bootstrapped = false, started = false;
    wifi_core_mode_t mode = WIFI_CORE_MODE_OFF;
    bool staConfigured = false, apConfigured = false;
    std::string staSsid, staPass, apSsid, apPass;
    int apChannel = 0, apMax = 4; bool apHidden = false;
    bool staNetif = false, apNetif = false;
    bool connecting = false, connected = false; Clock::time_point connectAt;
    Net cur{};
    bool dhcps = false, sharing = false;
    std::string apIp = "192.168.4.1", apMask = "255.255.255.0", apGw = "192.168.4.1";
    wifi_core_dhcp_config_t dhcpCfg{}; bool dhcpCfgSet = false;
    std::vector<wifi_core_client_t> clients;
};
std::mutex g_m;
State g_s;
std::vector<Net> g_nets;
const char *kStaIp = "10.0.2.15", *kStaMask = "255.255.255.0", *kStaGw = "10.0.2.2";

void defaultNets() {
    g_nets.clear();
    g_nets.push_back({"emu", "OPEN", "02:00:00:00:00:01", "", -45, 6});
}
struct Init { Init() { defaultNets(); } } g_init;

template <size_t N> void cp(char (&d)[N], const std::string &s) { strncpy(d, s.c_str(), N - 1); d[N - 1] = 0; }

/* resolve a pending async connect */
void staUpdate() {
    if (g_s.connecting && Clock::now() >= g_s.connectAt) {
        g_s.connecting = false;
        for (auto &n : g_nets) {
            if (n.ssid != g_s.staSsid) continue;
            if (n.auth == "OPEN" || n.password == g_s.staPass) { g_s.connected = true; g_s.cur = n; }
            break;
        }
    }
}
bool staHasIp() { staUpdate(); return g_s.connected && g_s.staNetif; }
bool staEnabled() { return g_s.mode == WIFI_CORE_MODE_STA || g_s.mode == WIFI_CORE_MODE_APSTA; }
bool apEnabled()  { return g_s.mode == WIFI_CORE_MODE_AP  || g_s.mode == WIFI_CORE_MODE_APSTA; }
void doConnect() { g_s.connecting = true; g_s.connected = false; g_s.connectAt = Clock::now() + std::chrono::milliseconds(400); }
}  // namespace

namespace emu { namespace internal {
void wifiReset() { std::lock_guard<std::mutex> l(g_m); g_s = State(); defaultNets(); }
void wifiAddNetwork(const char *ssid, const char *auth, int rssi, int channel, const char *password) {
    std::lock_guard<std::mutex> l(g_m);
    char mac[18]; snprintf(mac, sizeof mac, "02:00:00:00:00:%02X", (int)g_nets.size() + 1);
    g_nets.push_back({ssid, auth && *auth ? auth : (password && *password ? "WPA2_PSK" : "OPEN"), mac,
                      password ? password : "", rssi, channel});
}
}}  // namespace

extern "C" {

esp_err_t esp_wifi_set_max_tx_power(int8_t) { return ESP_OK; }

esp_err_t wifi_core_set_mode(const char *m) {
    std::lock_guard<std::mutex> l(g_m);
    g_s.bootstrapped = true;
    if (!m) return ESP_OK;
    wifi_core_mode_t v;
    if (!strcmp(m, "off")) v = WIFI_CORE_MODE_OFF;
    else if (!strcmp(m, "sta")) v = WIFI_CORE_MODE_STA;
    else if (!strcmp(m, "ap")) v = WIFI_CORE_MODE_AP;
    else if (!strcmp(m, "apsta")) v = WIFI_CORE_MODE_APSTA;
    else return ESP_ERR_INVALID_ARG;
    g_s.mode = v;
    return ESP_OK;
}
const char *wifi_core_get_mode(void) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_s.bootstrapped) return "off";
    switch (g_s.mode) { case WIFI_CORE_MODE_STA: return "sta"; case WIFI_CORE_MODE_AP: return "ap";
                        case WIFI_CORE_MODE_APSTA: return "apsta"; default: return "off"; }
}
esp_err_t wifi_core_start(void) {
    std::lock_guard<std::mutex> l(g_m);
    g_s.bootstrapped = true;
    if (g_s.mode == WIFI_CORE_MODE_OFF) { emu::log("wifi_core_start: mode is \"off\", call wifi.mode() first"); return ESP_ERR_INVALID_STATE; }
    if (staEnabled()) g_s.staNetif = true;
    if (apEnabled()) { g_s.apNetif = true; g_s.dhcps = true; }
    g_s.started = true;
    if (g_s.staConfigured && staEnabled()) doConnect();
    return ESP_OK;
}
esp_err_t wifi_core_stop(void) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_s.bootstrapped || !g_s.started) return ESP_OK;
    g_s.started = false; g_s.connected = g_s.connecting = false; g_s.dhcps = false;
    g_s.staNetif = g_s.apNetif = false; g_s.sharing = false; g_s.clients.clear();
    return ESP_OK;
}
bool wifi_core_is_started(void) { std::lock_guard<std::mutex> l(g_m); return g_s.started; }

static wifi_core_sta_info_t staInfoLocked() {
    wifi_core_sta_info_t i{};
    staUpdate();
    i.connected = g_s.connected && g_s.started;
    if (i.connected) {
        cp(i.ssid, g_s.cur.ssid); cp(i.bssid, g_s.cur.bssid); i.channel = (uint8_t)g_s.cur.channel;
        i.rssi = (int8_t)g_s.cur.rssi; cp(i.auth, g_s.cur.auth);
        i.has_ip = g_s.staNetif;
        if (i.has_ip) { cp(i.ip, kStaIp); cp(i.gateway, kStaGw); cp(i.netmask, kStaMask); }
    }
    return i;
}
static wifi_core_ap_info_t apInfoLocked() {
    wifi_core_ap_info_t a{};
    a.started = g_s.started && apEnabled();
    if (a.started) {
        cp(a.ssid, g_s.apSsid); a.channel = (uint8_t)(g_s.apChannel ? g_s.apChannel : 1);
        cp(a.ip, g_s.apIp); cp(a.netmask, g_s.apMask); a.clients = (int)g_s.clients.size();
    }
    return a;
}
wifi_core_status_t wifi_core_get_status(void) {
    std::lock_guard<std::mutex> l(g_m);
    wifi_core_status_t s{};
    s.mode = g_s.bootstrapped ? g_s.mode : WIFI_CORE_MODE_OFF;
    s.started = g_s.started;
    s.sta = staInfoLocked(); s.ap = apInfoLocked();
    return s;
}
esp_err_t wifi_core_scan(wifi_core_scan_result_t *out, int max, int *cnt) {
    std::lock_guard<std::mutex> l(g_m);
    g_s.bootstrapped = true;
    if (!g_s.started) return ESP_ERR_WIFI_NOT_STARTED;
    int n = 0;
    for (auto &net : g_nets) {
        if (n >= max || n >= WIFI_CORE_MAX_SCAN) break;
        auto &r = out[n++]; memset(&r, 0, sizeof r);
        cp(r.ssid, net.ssid); cp(r.bssid, net.bssid); r.rssi = (int8_t)net.rssi;
        r.channel = (uint8_t)net.channel; cp(r.auth, net.auth);
    }
    *cnt = n;
    return ESP_OK;
}
esp_err_t wifi_core_sta_config(const char *ssid, const char *pw) {
    std::lock_guard<std::mutex> l(g_m);
    g_s.bootstrapped = true;
    if (!ssid || !*ssid || strlen(ssid) > WIFI_CORE_MAX_SSID_LEN) return ESP_ERR_INVALID_ARG;
    size_t pl = pw ? strlen(pw) : 0;
    if (pl > WIFI_CORE_MAX_PASS_LEN || (pl > 0 && pl < 8)) return ESP_ERR_INVALID_ARG;
    g_s.staSsid = ssid; g_s.staPass = pw ? pw : ""; g_s.staConfigured = true;
    return ESP_OK;
}
esp_err_t wifi_core_sta_connect(void) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_s.started) return ESP_ERR_WIFI_NOT_STARTED;
    if (!g_s.staConfigured || !staEnabled()) return ESP_ERR_INVALID_STATE;
    doConnect();
    return ESP_OK;
}
esp_err_t wifi_core_sta_disconnect(void) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_s.started) return ESP_ERR_WIFI_NOT_STARTED;
    g_s.connected = g_s.connecting = false;
    return ESP_OK;
}
wifi_core_sta_info_t wifi_core_sta_get_info(void) { std::lock_guard<std::mutex> l(g_m); return staInfoLocked(); }

esp_err_t wifi_core_ap_config(const char *ssid, const char *pw, int ch, int maxc, bool hidden) {
    std::lock_guard<std::mutex> l(g_m);
    g_s.bootstrapped = true;
    if (!ssid || !*ssid || strlen(ssid) > WIFI_CORE_MAX_SSID_LEN) return ESP_ERR_INVALID_ARG;
    size_t pl = pw ? strlen(pw) : 0;
    if (pl > 0 && pl < 8) return ESP_ERR_INVALID_ARG;
    if (ch < 0 || ch > 13) return ESP_ERR_INVALID_ARG;
    if (maxc <= 0 || maxc > 10) maxc = 4;
    g_s.apSsid = ssid; g_s.apPass = pw ? pw : ""; g_s.apChannel = ch; g_s.apMax = maxc;
    g_s.apHidden = hidden; g_s.apConfigured = true;
    return ESP_OK;
}
esp_err_t wifi_core_ap_start(void) {
    if (!wifi_core_is_started()) return wifi_core_start();
    return ESP_OK;
}
esp_err_t wifi_core_ap_stop(void) {
    wifi_core_mode_t m; { std::lock_guard<std::mutex> l(g_m); m = g_s.mode; }
    if (m == WIFI_CORE_MODE_AP) return wifi_core_stop();
    if (m == WIFI_CORE_MODE_APSTA) return wifi_core_set_mode("sta");
    return ESP_OK;
}
wifi_core_ap_info_t wifi_core_ap_get_info(void) { std::lock_guard<std::mutex> l(g_m); return apInfoLocked(); }
esp_err_t wifi_core_ap_clients(wifi_core_client_t *out, int max, int *cnt) {
    std::lock_guard<std::mutex> l(g_m);
    int n = 0;
    for (auto &c : g_s.clients) { if (n >= max || n >= WIFI_CORE_MAX_CLIENTS) break; out[n++] = c; }
    *cnt = n;
    return ESP_OK;
}

esp_err_t wifi_core_dhcp_config(const wifi_core_dhcp_config_t *cfg) {
    std::lock_guard<std::mutex> l(g_m);
    if (!cfg) return ESP_ERR_INVALID_ARG;
    g_s.dhcpCfg = *cfg; g_s.dhcpCfgSet = true;
    if (cfg->ip[0]) g_s.apIp = cfg->ip;
    if (cfg->netmask[0]) g_s.apMask = cfg->netmask;
    if (cfg->gateway[0]) g_s.apGw = cfg->gateway;
    return ESP_OK;
}
esp_err_t wifi_core_dhcp_start(void) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_s.apNetif) return ESP_ERR_INVALID_STATE;
    g_s.dhcps = true; return ESP_OK;
}
esp_err_t wifi_core_dhcp_stop(void) { std::lock_guard<std::mutex> l(g_m); g_s.dhcps = false; return ESP_OK; }
bool wifi_core_dhcp_is_running(void) { std::lock_guard<std::mutex> l(g_m); return g_s.dhcps; }
esp_err_t wifi_core_dhcp_leases(wifi_core_lease_t *out, int max, int *cnt) {
    std::lock_guard<std::mutex> l(g_m);
    int n = 0;
    for (auto &c : g_s.clients) {
        if (n >= max || n >= WIFI_CORE_MAX_LEASES) break;
        if (!c.ip[0]) continue;
        memcpy(out[n].mac, c.mac, sizeof out[n].mac); memcpy(out[n].ip, c.ip, sizeof out[n].ip); n++;
    }
    *cnt = n;
    return ESP_OK;
}

static void fillNetif(wifi_core_netif_info_t &i, const char *name) {
    memset(&i, 0, sizeof i); cp(i.name, name);
    if (!strcmp(name, "sta")) {
        i.up = g_s.staNetif;
        if (staHasIp()) { cp(i.ip, kStaIp); cp(i.netmask, kStaMask); cp(i.gateway, kStaGw); }
    } else {
        i.up = g_s.apNetif;
        if (i.up) { cp(i.ip, g_s.apIp); cp(i.netmask, g_s.apMask); cp(i.gateway, g_s.apGw); }
    }
}
int wifi_core_net_interfaces(wifi_core_netif_info_t *out, int max) {
    std::lock_guard<std::mutex> l(g_m);
    int n = 0;
    if (g_s.staNetif && n < max) fillNetif(out[n++], "sta");
    if (g_s.apNetif && n < max) fillNetif(out[n++], "ap");
    return n;
}
esp_err_t wifi_core_net_info(const char *name, wifi_core_netif_info_t *out) {
    std::lock_guard<std::mutex> l(g_m);
    if (!name || (strcmp(name, "sta") && strcmp(name, "ap"))) return ESP_ERR_INVALID_ARG;
    if ((!strcmp(name, "sta") && !g_s.staNetif) || (!strcmp(name, "ap") && !g_s.apNetif)) return ESP_ERR_INVALID_STATE;
    fillNetif(*out, name);
    return ESP_OK;
}
const char *wifi_core_net_default_interface(void) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_s.sharing || (g_s.staNetif && !g_s.apNetif)) return g_s.staNetif ? "sta" : "none";
    if (g_s.apNetif && !g_s.staNetif) return "ap";
    if (g_s.staNetif) return "sta";
    return "none";
}
esp_err_t wifi_core_net_dns(const char *iface, char out_ip[16]) {
    std::lock_guard<std::mutex> l(g_m);
    out_ip[0] = 0;
    bool up = iface && ((!strcmp(iface, "sta") && g_s.staNetif) || (!strcmp(iface, "ap") && g_s.apNetif));
    if (!up) return ESP_ERR_INVALID_STATE;
    cp(*(char(*)[16])out_ip, !strcmp(iface, "sta") ? std::string(kStaGw) : g_s.apIp);
    return ESP_OK;
}
esp_err_t wifi_core_net_request(const char *iface, const char *host, int port, int timeout_ms,
                                wifi_core_request_result_t *out) {
    memset(out, 0, sizeof *out);
    if (iface) {
        std::lock_guard<std::mutex> l(g_m);
        bool up = (!strcmp(iface, "sta") && g_s.staNetif) || (!strcmp(iface, "ap") && g_s.apNetif);
        if (!up) { snprintf(out->error, sizeof out->error, "interface \"%s\" is not up", iface); return ESP_ERR_INVALID_STATE; }
        cp(out->local_ip, !strcmp(iface, "sta") ? std::string(kStaIp) : g_s.apIp);
    }
    sockaddr_in dest{}; dest.sin_family = AF_INET; dest.sin_port = htons((uint16_t)port);
    in_addr direct;
    if (inet_aton(host, &direct)) dest.sin_addr = direct;
    else {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; addrinfo *res = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
            snprintf(out->error, sizeof out->error, "DNS resolution failed for \"%s\"", host); return ESP_FAIL;
        }
        dest.sin_addr = ((sockaddr_in *)res->ai_addr)->sin_addr; freeaddrinfo(res);
    }
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, (sockaddr *)&dest, sizeof dest) != 0) {
        snprintf(out->error, sizeof out->error, "connect() failed: errno %d", errno); close(s); return ESP_FAIL;
    }
    close(s); out->success = true; return ESP_OK;
}
esp_err_t wifi_core_net_share(bool enable) {
    std::lock_guard<std::mutex> l(g_m);
    if (!enable) { g_s.sharing = false; return ESP_OK; }
    if (g_s.mode != WIFI_CORE_MODE_APSTA || !g_s.staNetif || !g_s.apNetif) return ESP_ERR_INVALID_STATE;
    if (!staHasIp()) return ESP_ERR_INVALID_STATE;
    g_s.sharing = true; return ESP_OK;
}
bool wifi_core_net_is_sharing(void) { std::lock_guard<std::mutex> l(g_m); return g_s.sharing; }
int wifi_core_signal_quality(int8_t rssi) { if (rssi >= -50) return 100; if (rssi <= -100) return 0; return 2 * (rssi + 100); }

/* --------------------------------------------------------------- DNS server */
}  // extern "C"

namespace {
struct Dns {
    bool running = false;
    std::vector<std::string> upstream{"8.8.8.8"};
    std::map<std::string, std::string> records;
    std::map<uint64_t, std::string> hashes;
    uint32_t cap = 0;
} g_dns;
std::mutex g_dm;
}
extern "C" {
esp_err_t dns_server_start(void) { std::lock_guard<std::mutex> l(g_dm); g_dns.running = true; return ESP_OK; }
esp_err_t dns_server_stop(void)  { std::lock_guard<std::mutex> l(g_dm); g_dns.running = false; return ESP_OK; }
bool dns_server_is_running(void) { std::lock_guard<std::mutex> l(g_dm); return g_dns.running; }
esp_err_t dns_server_set_upstream(const char *s[], int n) {
    if (n < 1 || n > DNS_SERVER_MAX_UPSTREAM) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> l(g_dm); g_dns.upstream.assign(s, s + n); return ESP_OK;
}
int dns_server_get_upstream(char out[][16], int max) {
    std::lock_guard<std::mutex> l(g_dm);
    int n = 0; for (auto &u : g_dns.upstream) { if (n >= max) break; strncpy(out[n], u.c_str(), 15); out[n][15] = 0; n++; }
    return n;
}
esp_err_t dns_server_add(const char *h, const char *ip) {
    std::lock_guard<std::mutex> l(g_dm);
    if (!g_dns.records.count(h) && g_dns.records.size() >= DNS_SERVER_MAX_RECORDS) return ESP_ERR_NO_MEM;
    g_dns.records[h] = ip; return ESP_OK;
}
esp_err_t dns_server_remove(const char *h) {
    std::lock_guard<std::mutex> l(g_dm); return g_dns.records.erase(h) ? ESP_OK : ESP_ERR_NOT_FOUND;
}
void dns_server_clear(void) { std::lock_guard<std::mutex> l(g_dm); g_dns.records.clear(); }
void dns_server_lookup(const char *h, char out[16]) {
    std::lock_guard<std::mutex> l(g_dm); auto it = g_dns.records.find(h);
    if (it == g_dns.records.end()) out[0] = 0; else { strncpy(out, it->second.c_str(), 15); out[15] = 0; }
}
esp_err_t dns_server_hash_init(uint32_t cap) {
    std::lock_guard<std::mutex> l(g_dm); g_dns.hashes.clear(); g_dns.cap = cap ? cap : DNS_SERVER_DEFAULT_HASH_CAPACITY; return ESP_OK;
}
esp_err_t dns_server_add_hash(uint64_t h, const char *ip) {
    std::lock_guard<std::mutex> l(g_dm);
    if (!g_dns.cap) g_dns.cap = DNS_SERVER_DEFAULT_HASH_CAPACITY;
    if (!g_dns.hashes.count(h) && g_dns.hashes.size() * 10 >= (size_t)g_dns.cap * 7) return ESP_ERR_NO_MEM;  // ~70% load
    g_dns.hashes[h] = ip; return ESP_OK;
}
esp_err_t dns_server_remove_hash(uint64_t h) {
    std::lock_guard<std::mutex> l(g_dm); return g_dns.hashes.erase(h) ? ESP_OK : ESP_ERR_NOT_FOUND;
}
void dns_server_hash_lookup(uint64_t h, char out[16]) {
    std::lock_guard<std::mutex> l(g_dm); auto it = g_dns.hashes.find(h);
    if (it == g_dns.hashes.end()) out[0] = 0; else { strncpy(out, it->second.c_str(), 15); out[15] = 0; }
}
uint32_t dns_server_hash_count(void)    { std::lock_guard<std::mutex> l(g_dm); return (uint32_t)g_dns.hashes.size(); }
uint32_t dns_server_hash_capacity(void) { std::lock_guard<std::mutex> l(g_dm); return g_dns.cap; }

/* HTTP server: not emulated (no endpoints are reachable); registrations are accepted and ignored. */
esp_err_t http_server_start(void) { return ESP_OK; }
esp_err_t http_server_stop(void) { return ESP_OK; }
esp_err_t http_server_add_get(const char *, esp_err_t (*)(httpd_req_t *)) { return ESP_OK; }
esp_err_t http_server_add_post(const char *, esp_err_t (*)(httpd_req_t *)) { return ESP_OK; }
}
