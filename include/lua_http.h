#ifndef LUA_HTTP_H
#define LUA_HTTP_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * wifi.http - HTTP/HTTPS client for Lua (esp_http_client + certificate bundle).
 *
 * Blocking:
 *   res, err = wifi.http.request{ url=..., method=..., headers=..., body=..., ... }
 *   res, err = wifi.http.get(url [, opts])
 *   res, err = wifi.http.post(url, body [, opts])
 *
 * Non-blocking (the request runs in its own task on core 0; Lua keeps running):
 *   id        = wifi.http.start(opts)            -- or nil, err
 *   res, err  = wifi.http.poll(id [, timeout_ms]) -- false while still running
 *   ok        = wifi.http.cancel(id)
 *
 * Options (all optional except url):
 *   url, method ("GET" "POST" "PUT" "PATCH" "DELETE" "HEAD"), headers = {name=value},
 *   body (string), timeout (ms, default 15000, total budget), max_body (bytes, default 65536,
 *   hard cap 1 MiB), redirects (default 5, 0 = do not follow), ca_cert (PEM string, to trust a
 *   private/self-signed server instead of the built-in certificate bundle)
 *
 * Result table: { status=200, ok=true, headers={lowercase-name=value}, body="...",
 *                 url="final url", redirects=0 }
 * Any HTTP status (404, 500, ...) is a normal result (ok=false). nil, err is only for
 * transport problems: no network, DNS, connect, TLS, timeout, body too large, bad options.
 *
 * Requires the STA interface to be connected with an IP.
 * Call wifi_http_register() from API_INIT() AFTER lua_wifi_register() (it adds wifi.http).
 */
void wifi_http_register(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* LUA_HTTP_H */
