#include <string.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "wrap.h"
#include "lua_wifi.h"
#include "wifi_core.h"
#include "dns_server.h"
#include "esp_err.h"
#include "http_server.h"
/*
 * This file is intentionally "dumb": every function here does
 *   1) pull arguments off the Lua stack,
 *   2) call exactly one wifi_core_* /dns_server_* function,
 *   3) push the result back as a Lua value/table.
 * All real network processing lives in wifi_core.c / dns_server.c.
 */

/* ---- shared table builders ------------------------------------------ */

static void push_sta_fields(lua_State *L, const wifi_core_sta_info_t *s)
{
    wrap_add_bool(L, "connected", s->connected);
    wrap_add_string(L, "ssid", s->ssid);
    wrap_add_string(L, "bssid", s->bssid);
    wrap_add_number(L, "channel", s->channel);
    wrap_add_number(L, "rssi", s->rssi);
    wrap_add_string(L, "auth", s->auth);
    if (s->has_ip) {
        wrap_add_string(L, "ip", s->ip);
        wrap_add_string(L, "gateway", s->gateway);
        wrap_add_string(L, "netmask", s->netmask);
    }
}

static void push_ap_fields(lua_State *L, const wifi_core_ap_info_t *a)
{
    wrap_add_bool(L, "started", a->started);
    wrap_add_string(L, "ssid", a->ssid);
    wrap_add_number(L, "channel", a->channel);
    if (a->started && a->ip[0]) {
        wrap_add_string(L, "ip", a->ip);
        wrap_add_string(L, "netmask", a->netmask);
    }
    wrap_add_number(L, "clients", a->clients);
}

/* ---- wifi.* top level ------------------------------------------------ */

static int l_wifi_mode(lua_State *L)
{
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        const char *m = luaL_checkstring(L, 1);
        esp_err_t err = wifi_core_set_mode(m);
        if (err != ESP_OK) {
            return luaL_error(L, "wifi.mode: expected \"off\"|\"sta\"|\"ap\"|\"apsta\" (%s)",
                               esp_err_to_name(err));
        }
    }
    wrap_return_string(L, wifi_core_get_mode());
    return 1;
}

static int l_wifi_start(lua_State *L)
{
    esp_err_t err = wifi_core_start();
    if (err != ESP_OK) {
        lua_pushboolean(L, false);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    wrap_return_bool(L, true);
    return 1;
}

static int l_wifi_stop(lua_State *L)
{
    wrap_return_bool(L, wifi_core_stop() == ESP_OK);
    return 1;
}

static int l_wifi_status(lua_State *L)
{
    wifi_core_status_t st = wifi_core_get_status();
    wrap_table_start(L);
    wrap_add_string(L, "mode", wifi_core_get_mode());
    wrap_add_bool(L, "started", st.started);
    wrap_add_table(L);
    push_sta_fields(L, &st.sta);
    wrap_end_table(L, "sta");
    wrap_add_table(L);
    push_ap_fields(L, &st.ap);
    wrap_end_table(L, "ap");
    return 1;
}

static int l_wifi_scan(lua_State *L)
{
    wifi_core_scan_result_t results[WIFI_CORE_MAX_SCAN];
    int count = 0;
    esp_err_t err = wifi_core_scan(results, WIFI_CORE_MAX_SCAN, &count);
    if (err != ESP_OK) {
        return luaL_error(L, "wifi.scan failed: %s", esp_err_to_name(err));
    }
    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        lua_newtable(L);
        wrap_add_string(L, "ssid", results[i].ssid);
        wrap_add_string(L, "bssid", results[i].bssid);
        wrap_add_number(L, "rssi", results[i].rssi);
        wrap_add_number(L, "channel", results[i].channel);
        wrap_add_string(L, "auth", results[i].auth);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ---- wifi.sta.* -------------------------------------------------------*/

static int l_sta_config(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "ssid");
    const char *ssid = luaL_checkstring(L, -1);
    lua_getfield(L, 1, "password");
    const char *password = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    esp_err_t err = wifi_core_sta_config(ssid, password);
    lua_pop(L, 2);
    if (err != ESP_OK) {
        return luaL_error(L, "wifi.sta.config: invalid ssid/password (password must be empty or 8-63 chars)");
    }
    wrap_return_bool(L, true);
    return 1;
}

static int l_sta_connect(lua_State *L)
{
    wrap_return_bool(L, wifi_core_sta_connect() == ESP_OK);
    return 1;
}

static int l_sta_disconnect(lua_State *L)
{
    wrap_return_bool(L, wifi_core_sta_disconnect() == ESP_OK);
    return 1;
}

static int l_sta_status(lua_State *L)
{
    wifi_core_sta_info_t s = wifi_core_sta_get_info();
    wrap_table_start(L);
    push_sta_fields(L, &s);
    return 1;
}

static int l_sta_rssi(lua_State *L)
{
    wifi_core_sta_info_t s = wifi_core_sta_get_info();
    if (!s.connected) { wrap_return_nil(L); return 1; }
    wrap_return_number(L, s.rssi);
    return 1;
}

static int l_sta_ip(lua_State *L)
{
    wifi_core_sta_info_t s = wifi_core_sta_get_info();
    if (!s.has_ip) { wrap_return_nil(L); return 1; }
    wrap_return_string(L, s.ip);
    return 1;
}

static int l_sta_gateway(lua_State *L)
{
    wifi_core_sta_info_t s = wifi_core_sta_get_info();
    if (!s.has_ip) { wrap_return_nil(L); return 1; }
    wrap_return_string(L, s.gateway);
    return 1;
}

static int l_sta_dns(lua_State *L)
{
    char ip[16];
    if (wifi_core_net_dns("sta", ip) != ESP_OK || ip[0] == '\0') {
        wrap_return_nil(L);
        return 1;
    }
    wrap_return_string(L, ip);
    return 1;
}

/* ---- wifi.ap.* -------------------------------------------------------*/

static int l_ap_config(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "ssid");
    const char *ssid = luaL_checkstring(L, -1);
    lua_getfield(L, 1, "password");
    const char *password = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_getfield(L, 1, "channel");
    int channel = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 1;
    lua_getfield(L, 1, "max_connections");
    int max_conn = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 4;
    lua_getfield(L, 1, "hidden");
    bool hidden = lua_toboolean(L, -1);

    esp_err_t err = wifi_core_ap_config(ssid, password, channel, max_conn, hidden);
    lua_pop(L, 5);
    if (err != ESP_OK) {
        return luaL_error(L, "wifi.ap.config: invalid parameters (ssid/password/channel)");
    }
    wrap_return_bool(L, true);
    return 1;
}

static int l_ap_start(lua_State *L)
{
    wrap_return_bool(L, wifi_core_ap_start() == ESP_OK);
    return 1;
}

static int l_ap_stop(lua_State *L)
{
    wrap_return_bool(L, wifi_core_ap_stop() == ESP_OK);
    return 1;
}

static int l_ap_status(lua_State *L)
{
    wifi_core_ap_info_t a = wifi_core_ap_get_info();
    wrap_table_start(L);
    push_ap_fields(L, &a);
    return 1;
}

static int l_ap_ip(lua_State *L)
{
    wifi_core_ap_info_t a = wifi_core_ap_get_info();
    if (!a.started || a.ip[0] == '\0') { wrap_return_nil(L); return 1; }
    wrap_return_string(L, a.ip);
    return 1;
}

static int l_ap_dhcp(lua_State *L)
{
    wrap_return_bool(L, wifi_core_dhcp_is_running());
    return 1;
}

static int l_ap_clients(lua_State *L)
{
    wifi_core_client_t list[WIFI_CORE_MAX_CLIENTS];
    int n = 0;
    wifi_core_ap_clients(list, WIFI_CORE_MAX_CLIENTS, &n);
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        lua_newtable(L);
        wrap_add_string(L, "mac", list[i].mac);
        if (list[i].ip[0]) wrap_add_string(L, "ip", list[i].ip);
        wrap_add_number(L, "rssi", list[i].rssi);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ---- wifi.dhcp.* -------------------------------------------------------*/

static int l_dhcp_config(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    wifi_core_dhcp_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    lua_getfield(L, 1, "ip");
    if (lua_isstring(L, -1)) strncpy(cfg.ip, lua_tostring(L, -1), sizeof(cfg.ip) - 1);
    lua_getfield(L, 1, "netmask");
    if (lua_isstring(L, -1)) strncpy(cfg.netmask, lua_tostring(L, -1), sizeof(cfg.netmask) - 1);
    lua_getfield(L, 1, "gateway");
    if (lua_isstring(L, -1)) strncpy(cfg.gateway, lua_tostring(L, -1), sizeof(cfg.gateway) - 1);
    lua_getfield(L, 1, "start");
    const char *start_ip = luaL_checkstring(L, -1);
    strncpy(cfg.start_ip, start_ip, sizeof(cfg.start_ip) - 1);
    /* NOTE: `end` is a reserved word in Lua and CANNOT be written as
     * `end = "..."` inside a table constructor -- that is a Lua syntax
     * error. Use `["end"] = "192.168.4.100"` instead. lua_getfield()
     * below works with either, since it just looks up the string key. */
    lua_getfield(L, 1, "end");
    const char *end_ip = luaL_checkstring(L, -1);
    strncpy(cfg.end_ip, end_ip, sizeof(cfg.end_ip) - 1);
    lua_getfield(L, 1, "lease_time");
    lua_Integer lease_sec = lua_isnumber(L, -1) ? lua_tointeger(L, -1) : 0;
    cfg.lease_time_min = lease_sec > 0 ? (uint32_t)(lease_sec / 60) : 0;

    lua_pop(L, 6);

    esp_err_t err = wifi_core_dhcp_config(&cfg);
    wrap_return_bool(L, err == ESP_OK);
    return 1;
}

static int l_dhcp_start(lua_State *L)
{
    wrap_return_bool(L, wifi_core_dhcp_start() == ESP_OK);
    return 1;
}

static int l_dhcp_stop(lua_State *L)
{
    wrap_return_bool(L, wifi_core_dhcp_stop() == ESP_OK);
    return 1;
}

static int l_dhcp_status(lua_State *L)
{
    wrap_table_start(L);
    wrap_add_bool(L, "running", wifi_core_dhcp_is_running());
    return 1;
}

static int l_dhcp_leases(lua_State *L)
{
    wifi_core_lease_t leases[WIFI_CORE_MAX_LEASES];
    int n = 0;
    wifi_core_dhcp_leases(leases, WIFI_CORE_MAX_LEASES, &n);
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        lua_newtable(L);
        wrap_add_string(L, "mac", leases[i].mac);
        wrap_add_string(L, "ip", leases[i].ip);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
static esp_err_t restart_handler(httpd_req_t *req) {
    http_server_stop();
    wifi_core_stop();
    esp_restart();
}
/* ---- wifi.dns.* -------------------------------------------------------*/

static int l_dns_start(lua_State *L)
{
    http_server_start();
    http_server_add_get("restart",restart_handler);
    wrap_return_bool(L, dns_server_start() == ESP_OK);
    return 1;
}

static int l_dns_stop(lua_State *L)
{
    wrap_return_bool(L, dns_server_stop() == ESP_OK);
    return 1;
}

static int l_dns_status(lua_State *L)
{
    wrap_table_start(L);
    wrap_add_bool(L, "running", dns_server_is_running());
    return 1;
}

static int l_dns_upstream(lua_State *L)
{
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        char servers[DNS_SERVER_MAX_UPSTREAM][16];
        int n = dns_server_get_upstream(servers, DNS_SERVER_MAX_UPSTREAM);
        lua_newtable(L);
        for (int i = 0; i < n; i++) {
            lua_pushstring(L, servers[i]);
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }

    luaL_checktype(L, 1, LUA_TTABLE);
    const char *list[DNS_SERVER_MAX_UPSTREAM];
    int n = 0;
    lua_Integer len = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= len && n < DNS_SERVER_MAX_UPSTREAM; i++) {
        lua_geti(L, 1, i);
        if (lua_isstring(L, -1)) list[n++] = lua_tostring(L, -1);
        /* leave the string on the stack -- it stays valid until the
         * dns_server_set_upstream() call below copies it out */
    }
    esp_err_t err = dns_server_set_upstream(list, n);
    lua_pop(L, n);
    wrap_return_bool(L, err == ESP_OK);
    return 1;
}

static int l_dns_add(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    const char *ip = luaL_checkstring(L, 2);
    wrap_return_bool(L, dns_server_add(host, ip) == ESP_OK);
    return 1;
}

static int l_dns_remove(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    wrap_return_bool(L, dns_server_remove(host) == ESP_OK);
    return 1;
}

static int l_dns_clear(lua_State *L)
{
    dns_server_clear();
    return 0;
}

static int l_dns_lookup(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    char ip[16];
    dns_server_lookup(host, ip);
    if (ip[0] == '\0') { wrap_return_nil(L); return 1; }
    wrap_return_string(L, ip);
    return 1;
}

static int l_dns_add_hash(lua_State *L)
{
    /* lua_Integer is 64-bit in standard (non-LUA_32BITS) Lua 5.4 builds,
     * so this carries the full FNV-1a 64-bit hash without truncation.
     * A hex literal like 0xcbf29ce484222325 that Lua reads as a
     * "negative" 64-bit integer is fine -- the (uint64_t) cast below
     * reinterprets the same bit pattern, which is exactly the hash. */
    uint64_t hash = (uint64_t)luaL_checkinteger(L, 1);
    const char *ip = luaL_checkstring(L, 2);
    esp_err_t err = dns_server_add_hash(hash, ip);
    if (err != ESP_OK) {
        lua_pushboolean(L, false);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    wrap_return_bool(L, true);
    return 1;
}

static int l_dns_hash_lookup(lua_State *L)
{
    uint64_t hash = (uint64_t)luaL_checkinteger(L, 1);
    char ip[16];
    dns_server_hash_lookup(hash, ip);
    if (ip[0] == '\0') { wrap_return_nil(L); return 1; }
    wrap_return_string(L, ip);
    return 1;
}

static int l_dns_hash_init(lua_State *L)
{
    uint32_t capacity = (uint32_t)luaL_checkinteger(L, 1);
    wrap_return_bool(L, dns_server_hash_init(capacity) == ESP_OK);
    return 1;
}

static int l_dns_hash_count(lua_State *L)
{
    wrap_return_number(L, dns_server_hash_count());
    return 1;
}

static int l_dns_hash_capacity(lua_State *L)
{
    wrap_return_number(L, dns_server_hash_capacity());
    return 1;
}

/* ---- wifi.net.* -------------------------------------------------------*/

static void push_netif_fields(lua_State *L, const wifi_core_netif_info_t *n)
{
    wrap_add_string(L, "name", n->name);
    wrap_add_bool(L, "up", n->up);
    if (n->ip[0]) {
        wrap_add_string(L, "ip", n->ip);
        wrap_add_string(L, "netmask", n->netmask);
        wrap_add_string(L, "gateway", n->gateway);
    }
}

static int l_net_interfaces(lua_State *L)
{
    wifi_core_netif_info_t list[2];
    int n = wifi_core_net_interfaces(list, 2);
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        lua_newtable(L);
        push_netif_fields(L, &list[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_net_info(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    wifi_core_netif_info_t info;
    if (wifi_core_net_info(name, &info) != ESP_OK) { wrap_return_nil(L); return 1; }
    wrap_table_start(L);
    push_netif_fields(L, &info);
    return 1;
}

static int l_net_ip(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    wifi_core_netif_info_t info;
    if (wifi_core_net_info(name, &info) != ESP_OK || info.ip[0] == '\0') { wrap_return_nil(L); return 1; }
    wrap_return_string(L, info.ip);
    return 1;
}

static int l_net_gateway(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    wifi_core_netif_info_t info;
    if (wifi_core_net_info(name, &info) != ESP_OK || info.gateway[0] == '\0') { wrap_return_nil(L); return 1; }
    wrap_return_string(L, info.gateway);
    return 1;
}

static int l_net_dns(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    char ip[16];
    if (wifi_core_net_dns(name, ip) != ESP_OK || ip[0] == '\0') { wrap_return_nil(L); return 1; }
    wrap_return_string(L, ip);
    return 1;
}

static int l_net_route(lua_State *L)
{
    wrap_return_string(L, wifi_core_net_default_interface());
    return 1;
}

static int l_net_interface(lua_State *L)
{
    /* Not a general "bind all future sockets to this interface" switch
     * -- ESP-IDF/lwIP has no per-thread interface selection. This picks
     * which netif lwIP treats as the *default route* (esp_netif_set_default_netif),
     * i.e. the interface used when a destination doesn't match a more
     * specific route. See wifi.net.request() for per-connection
     * interface selection instead. */
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        wrap_return_string(L, wifi_core_net_default_interface());
        return 1;
    }
    const char *name = luaL_checkstring(L, 1);
    wifi_core_netif_info_t info;
    if (wifi_core_net_info(name, &info) != ESP_OK) {
        wrap_return_bool(L, false);
        return 1;
    }
    wrap_return_bool(L, true);
    (void)info;
    return 1;
}

static int l_net_share(lua_State *L)
{
    bool enable = lua_gettop(L) >= 1 ? lua_toboolean(L, 1) : true;
    esp_err_t err = wifi_core_net_share(enable);
    if (err != ESP_OK) {
        lua_pushboolean(L, false);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    wrap_return_bool(L, true);
    return 1;
}

static int l_net_sharing(lua_State *L)
{
    wrap_return_bool(L, wifi_core_net_is_sharing());
    return 1;
}

static int l_net_request(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "interface");
    const char *iface = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_getfield(L, 1, "host");
    const char *host = luaL_checkstring(L, -1);
    lua_getfield(L, 1, "port");
    int port = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 80;
    lua_getfield(L, 1, "timeout");
    int timeout_ms = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 5000;

    wifi_core_request_result_t result;
    wifi_core_net_request(iface, host, port, timeout_ms, &result);
    lua_pop(L, 4);

    wrap_table_start(L);
    wrap_add_bool(L, "success", result.success);
    if (result.local_ip[0]) wrap_add_string(L, "local_ip", result.local_ip);
    if (!result.success) wrap_add_string(L, "error", result.error);
    return 1;
}

/* ---- wifi.signal.* -----------------------------------------------------*/

static int l_signal_rssi(lua_State *L) { return l_sta_rssi(L); }

static int l_signal_quality(lua_State *L)
{
    wifi_core_sta_info_t s = wifi_core_sta_get_info();
    if (!s.connected) { wrap_return_nil(L); return 1; }
    wrap_return_number(L, wifi_core_signal_quality(s.rssi));
    return 1;
}

static int l_signal_info(lua_State *L)
{
    wifi_core_sta_info_t s = wifi_core_sta_get_info();
    wrap_table_start(L);
    if (s.connected) {
        wrap_add_number(L, "rssi", s.rssi);
        wrap_add_number(L, "quality", wifi_core_signal_quality(s.rssi));
    }
    return 1;
}
static int l_tx_power(lua_State *L) {
    uint8_t power = (uint8_t)luaL_checkinteger(L,1);
    EEC(esp_wifi_set_max_tx_power(power));
    return 0;
}
/* ---- registration ------------------------------------------------------*/

void lua_wifi_register(lua_State *L)
{
    wrap_table_start(L); /* wifi */
    wrap_add_function(L, "mode", l_wifi_mode);
    wrap_add_function(L, "start", l_wifi_start);
    wrap_add_function(L, "stop", l_wifi_stop);
    wrap_add_function(L, "status", l_wifi_status);
    wrap_add_function(L, "scan", l_wifi_scan);

    wrap_add_table(L); /* wifi.sta */
    wrap_add_function(L, "config", l_sta_config);
    wrap_add_function(L, "connect", l_sta_connect);
    wrap_add_function(L, "disconnect", l_sta_disconnect);
    wrap_add_function(L, "status", l_sta_status);
    wrap_add_function(L, "rssi", l_sta_rssi);
    wrap_add_function(L, "ip", l_sta_ip);
    wrap_add_function(L, "gateway", l_sta_gateway);
    wrap_add_function(L, "dns", l_sta_dns);
    wrap_add_function(L, "info", l_sta_status);
    wrap_end_table(L, "sta");

    wrap_add_table(L); /* wifi.ap */
    wrap_add_function(L, "config", l_ap_config);
    wrap_add_function(L, "start", l_ap_start);
    wrap_add_function(L, "stop", l_ap_stop);
    wrap_add_function(L, "status", l_ap_status);
    wrap_add_function(L, "clients", l_ap_clients);
    wrap_add_function(L, "ip", l_ap_ip);
    wrap_add_function(L, "dhcp", l_ap_dhcp);
    wrap_add_function(L, "info", l_ap_status);
    wrap_end_table(L, "ap");

    wrap_add_table(L); /* wifi.dhcp */
    wrap_add_function(L, "config", l_dhcp_config);
    wrap_add_function(L, "start", l_dhcp_start);
    wrap_add_function(L, "stop", l_dhcp_stop);
    wrap_add_function(L, "status", l_dhcp_status);
    wrap_add_function(L, "leases", l_dhcp_leases);
    wrap_end_table(L, "dhcp");

    wrap_add_table(L); /* wifi.dns */
    wrap_add_function(L, "start", l_dns_start);
    wrap_add_function(L, "stop", l_dns_stop);
    wrap_add_function(L, "status", l_dns_status);
    wrap_add_function(L, "upstream", l_dns_upstream);
    wrap_add_function(L, "add", l_dns_add);
    wrap_add_function(L, "remove", l_dns_remove);
    wrap_add_function(L, "clear", l_dns_clear);
    wrap_add_function(L, "lookup", l_dns_lookup);
    wrap_add_function(L, "add_hash", l_dns_add_hash);
    wrap_add_function(L, "hash_lookup", l_dns_hash_lookup);
    wrap_add_function(L, "hash_init", l_dns_hash_init);
    wrap_add_function(L, "hash_count", l_dns_hash_count);
    wrap_add_function(L, "hash_capacity", l_dns_hash_capacity);
    wrap_end_table(L, "dns");

    wrap_add_table(L); /* wifi.net */
    wrap_add_function(L, "interfaces", l_net_interfaces);
    wrap_add_function(L, "info", l_net_info);
    wrap_add_function(L, "ip", l_net_ip);
    wrap_add_function(L, "gateway", l_net_gateway);
    wrap_add_function(L, "dns", l_net_dns);
    wrap_add_function(L, "route", l_net_route);
    wrap_add_function(L, "request", l_net_request);
    wrap_add_function(L, "interface", l_net_interface);
    wrap_add_function(L, "share", l_net_share);
    wrap_add_function(L, "sharing", l_net_sharing);
    wrap_end_table(L, "net");

    wrap_add_table(L); /* wifi.signal */
    wrap_add_function(L, "rssi", l_signal_rssi);
    wrap_add_function(L, "quality", l_signal_quality);
    wrap_add_function(L, "info", l_signal_info);
    wrap_add_function(L,"setPower",l_tx_power);
    wrap_end_table(L, "signal");

    lua_setglobal(L, "wifi");
}
