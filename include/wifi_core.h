#ifndef WIFI_CORE_H
#define WIFI_CORE_H

/*
 * wifi_core: ESP-IDF Wi-Fi / esp_netif / lwIP systems layer.
 *
 * This module owns ALL real network state and processing:
 *   - esp_wifi / esp_netif / esp_event lifecycle
 *   - Wi-Fi event handling (STA connect/disconnect, AP clients, scan done)
 *   - DHCP server (esp_netif's built-in lwIP dhcps) configuration
 *   - Network interface enumeration
 *
 * Nothing here ever touches a lua_State. It is safe to call from any
 * FreeRTOS task. Every "get" function returns a snapshot copy taken
 * under an internal mutex, so callers never see a struct being mutated
 * concurrently by the Wi-Fi event handler.
 *
 * The Lua binding layer (lua_wifi.c) is a thin adapter on top of this.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_CORE_MAX_SSID_LEN   32
#define WIFI_CORE_MAX_PASS_LEN   64
#define WIFI_CORE_MAX_SCAN       32
#define WIFI_CORE_MAX_CLIENTS    16
#define WIFI_CORE_MAX_LEASES     16

typedef enum {
    WIFI_CORE_MODE_OFF = 0,
    WIFI_CORE_MODE_STA,
    WIFI_CORE_MODE_AP,
    WIFI_CORE_MODE_APSTA,
} wifi_core_mode_t;

typedef struct {
    bool connected;
    char ssid[WIFI_CORE_MAX_SSID_LEN + 1];
    char bssid[18];         /* "AA:BB:CC:DD:EE:FF" */
    uint8_t channel;
    int8_t rssi;
    char auth[24];
    char ip[16];
    char gateway[16];
    char netmask[16];
    bool has_ip;
} wifi_core_sta_info_t;

typedef struct {
    bool started;
    char ssid[WIFI_CORE_MAX_SSID_LEN + 1];
    uint8_t channel;
    char ip[16];
    char netmask[16];
    int clients;
} wifi_core_ap_info_t;

typedef struct {
    wifi_core_mode_t mode;
    bool started;
    wifi_core_sta_info_t sta;
    wifi_core_ap_info_t ap;
} wifi_core_status_t;

typedef struct {
    char ssid[WIFI_CORE_MAX_SSID_LEN + 1];
    char bssid[18];
    int8_t rssi;
    uint8_t channel;
    char auth[24];
} wifi_core_scan_result_t;

typedef struct {
    char mac[18];
    char ip[16];   /* "" if not yet known */
    int8_t rssi;
} wifi_core_client_t;

typedef struct {
    char mac[18];
    char ip[16];
} wifi_core_lease_t;

typedef struct {
    char name[8];      /* "sta" / "ap" */
    bool up;
    char ip[16];
    char netmask[16];
    char gateway[16];
} wifi_core_netif_info_t;

typedef struct {
    char ip[16];        /* dhcps pool start (informational echo of config) */
} wifi_core_dhcp_status_t;

typedef struct {
    char ip[16];         /* "" = leave AP static IP unchanged */
    char netmask[16];
    char gateway[16];
    char start_ip[16];
    char end_ip[16];
    uint32_t lease_time_min; /* 0 = leave at ESP-IDF default */
} wifi_core_dhcp_config_t;

typedef struct {
    bool success;
    char local_ip[16];
    char error[80];
} wifi_core_request_result_t;

/* ---- system lifecycle --------------------------------------------- */

/* Sets the desired mode. Does not start the driver (call wifi_core_start()
 * for that) unless the driver is already started, in which case the mode
 * switch is applied live. Returns the currently configured mode if
 * mode_str is NULL. */
esp_err_t wifi_core_set_mode(const char *mode_str);
const char *wifi_core_get_mode(void);

esp_err_t wifi_core_start(void);
esp_err_t wifi_core_stop(void);
bool wifi_core_is_started(void);

wifi_core_status_t wifi_core_get_status(void);

/* Blocking scan (runs on the calling task; ESP-IDF's own scan
 * synchronization is used internally -- see wifi_core.c for why this is
 * not a Lua-visible polling loop). */
esp_err_t wifi_core_scan(wifi_core_scan_result_t *out, int max, int *out_count);

/* ---- STA ------------------------------------------------------------ */

esp_err_t wifi_core_sta_config(const char *ssid, const char *password);
esp_err_t wifi_core_sta_connect(void);
esp_err_t wifi_core_sta_disconnect(void);
wifi_core_sta_info_t wifi_core_sta_get_info(void);

/* ---- AP --------------------------------------------------------------*/

esp_err_t wifi_core_ap_config(const char *ssid, const char *password,
                               int channel, int max_conn, bool hidden);
esp_err_t wifi_core_ap_start(void);
esp_err_t wifi_core_ap_stop(void);
wifi_core_ap_info_t wifi_core_ap_get_info(void);
esp_err_t wifi_core_ap_clients(wifi_core_client_t *out, int max, int *out_count);

/* ---- DHCP server (on the AP netif) ---------------------------------- */

esp_err_t wifi_core_dhcp_config(const wifi_core_dhcp_config_t *cfg);
esp_err_t wifi_core_dhcp_start(void);
esp_err_t wifi_core_dhcp_stop(void);
bool wifi_core_dhcp_is_running(void);
esp_err_t wifi_core_dhcp_leases(wifi_core_lease_t *out, int max, int *out_count);

/* ---- Network interfaces ---------------------------------------------*/

int wifi_core_net_interfaces(wifi_core_netif_info_t *out, int max);
esp_err_t wifi_core_net_info(const char *name, wifi_core_netif_info_t *out);
const char *wifi_core_net_default_interface(void);
esp_err_t wifi_core_net_dns(const char *iface, char out_ip[16]);

/* Best-effort interface-bound TCP connect probe. See wifi_core.c for the
 * documented limitations of interface selection on lwIP/ESP-IDF. */
esp_err_t wifi_core_net_request(const char *iface, const char *host, int port,
                                 int timeout_ms, wifi_core_request_result_t *out);

/* ---- Internet sharing (NAT / NAPT, AP clients -> out via STA) --------
 *
 * Requires mode "apsta" with STA connected and holding an IP. Enables
 * esp_netif_napt_enable() on the AP netif and makes STA the default
 * route, so traffic from SoftAP clients is address-translated and
 * forwarded out through the STA link (like a Wi-Fi repeater/router).
 * Requires CONFIG_LWIP_IP_FORWARD + CONFIG_LWIP_IPV4_NAPT to be enabled
 * in sdkconfig (both are now on for this project). */
esp_err_t wifi_core_net_share(bool enable);
bool wifi_core_net_is_sharing(void);

/* ---- Signal ------------------------------------------------------------*/

int wifi_core_signal_quality(int8_t rssi);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CORE_H */
