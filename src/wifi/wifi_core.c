#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "wrap.h"
#include "wifi_core.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "dhcpserver/dhcpserver.h"

static const char *TAG = "wifi_core";

/* ---------------------------------------------------------------------
 * Internal state. Every field here is only ever written from:
 *   - a public wifi_core_* call (always executed on whichever task calls
 *     it -- in this project that is always the "lua" task), or
 *   - the Wi-Fi/IP event handler, which runs on the esp_event default
 *     event loop task.
 * So it is a genuine 2-writer situation and every access is protected by
 * s_mutex. Locks are held only for plain struct field copies -- never
 * across an esp_wifi_* /esp_netif_* call.
 * ------------------------------------------------------------------- */

typedef struct {
    wifi_core_mode_t mode;
    bool started;

    bool sta_configured;
    wifi_config_t sta_cfg;
    bool sta_connected;
    bool sta_has_ip;
    wifi_ap_record_t sta_apinfo;
    esp_netif_ip_info_t sta_ip;

    bool ap_configured;
    wifi_config_t ap_cfg;
    bool ap_started;
    int ap_client_count;

    bool dhcps_running;
} wifi_core_internal_t;

static wifi_core_internal_t s_st;
static SemaphoreHandle_t s_mutex;
static bool s_bootstrapped = false;

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_sharing = false; /* NAPT internet-sharing (AP -> STA) state */

/* ---------------------------------------------------------------------
 * Bootstrap: esp_netif_init / default event loop / esp_wifi_init / NVS.
 * Idempotent, called lazily from the first mode/start call so this file
 * can be dropped into the project without touching app_main().
 * ------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data);
static void ip_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data);

static esp_err_t wifi_core_ensure_bootstrap(void)
{
    if (s_bootstrapped) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.mode = WIFI_CORE_MODE_OFF;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, NULL));

    /* Wi-Fi config is applied explicitly by us -- do not let ESP-IDF
     * restore whatever was last saved to NVS behind our back. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    
    s_bootstrapped = true;
    ESP_LOGI(TAG, "bootstrap complete");
    return ESP_OK;
}

#define LOCK()   xSemaphoreTake(s_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mutex)

/* ---------------------------------------------------------------------
 * Event handling -- this is the ONLY place Wi-Fi/IP events are consumed.
 * Handlers run on the esp_event task, never on the Lua task, and never
 * call into Lua. They just update the mutex-protected snapshot.
 * ------------------------------------------------------------------- */

static const char *auth_mode_to_str(wifi_auth_mode_t m)
{
    switch (m) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA_WPA2_PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2_WPA3_PSK";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI_PSK";
        case WIFI_AUTH_OWE:             return "OWE";
        default:                        return "UNKNOWN";
    }
}

static void mac_to_str(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    (void)arg; (void)base;

    switch (id) {
        case WIFI_EVENT_STA_CONNECTED: {
            LOCK();
            s_st.sta_connected = true;
            esp_wifi_sta_get_ap_info(&s_st.sta_apinfo);
            UNLOCK();
            ESP_LOGI(TAG, "STA connected");
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            LOCK();
            s_st.sta_connected = false;
            s_st.sta_has_ip = false;
            memset(&s_st.sta_apinfo, 0, sizeof(s_st.sta_apinfo));
            UNLOCK();
            ESP_LOGW(TAG, "STA disconnected");
            break;
        }
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            break;
        case WIFI_EVENT_STA_STOP:
            ESP_LOGI(TAG, "STA stopped");
            LOCK();
            s_st.sta_connected = false;
            s_st.sta_has_ip = false;
            UNLOCK();
            break;
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP started");
            LOCK(); s_st.ap_started = true; UNLOCK();
            break;
        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "AP stopped");
            LOCK(); s_st.ap_started = false; s_st.ap_client_count = 0; UNLOCK();
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            char macstr[18];
            mac_to_str(e->mac, macstr);
            ESP_LOGI(TAG, "AP: station connected %s", macstr);
            LOCK(); s_st.ap_client_count++; UNLOCK();
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
            char macstr[18];
            mac_to_str(e->mac, macstr);
            ESP_LOGI(TAG, "AP: station disconnected %s", macstr);
            LOCK();
            if (s_st.ap_client_count > 0) s_st.ap_client_count--;
            UNLOCK();
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "scan done");
            break;
        default:
            break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        LOCK();
        s_st.sta_has_ip = true;
        s_st.sta_ip = e->ip_info;
        UNLOCK();
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&e->ip_info.ip));
    } else if (id == IP_EVENT_STA_LOST_IP) {
        LOCK();
        s_st.sta_has_ip = false;
        UNLOCK();
        ESP_LOGW(TAG, "STA lost IP");
    }
}

/* ---------------------------------------------------------------------
 * Mode / start / stop
 * ------------------------------------------------------------------- */

static wifi_mode_t core_mode_to_esp(wifi_core_mode_t m)
{
    switch (m) {
        case WIFI_CORE_MODE_STA:   return WIFI_MODE_STA;
        case WIFI_CORE_MODE_AP:    return WIFI_MODE_AP;
        case WIFI_CORE_MODE_APSTA: return WIFI_MODE_APSTA;
        default:                   return WIFI_MODE_NULL;
    }
}

esp_err_t wifi_core_set_mode(const char *mode_str)
{
    esp_err_t err = wifi_core_ensure_bootstrap();
    if (err != ESP_OK) return err;

    if (mode_str == NULL) {
        return ESP_OK; /* caller just wants wifi_core_get_mode() */
    }

    wifi_core_mode_t m;
    if (strcmp(mode_str, "off") == 0) m = WIFI_CORE_MODE_OFF;
    else if (strcmp(mode_str, "sta") == 0) m = WIFI_CORE_MODE_STA;
    else if (strcmp(mode_str, "ap") == 0) m = WIFI_CORE_MODE_AP;
    else if (strcmp(mode_str, "apsta") == 0) m = WIFI_CORE_MODE_APSTA;
    else return ESP_ERR_INVALID_ARG;

    LOCK();
    s_st.mode = m;
    bool started = s_st.started;
    UNLOCK();

    if (started) {
        /* live mode switch while running */
        err = esp_wifi_set_mode(core_mode_to_esp(m));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        }
    }
    return err;
}

const char *wifi_core_get_mode(void)
{
    if (!s_bootstrapped) return "off";
    LOCK();
    wifi_core_mode_t m = s_st.mode;
    UNLOCK();
    switch (m) {
        case WIFI_CORE_MODE_STA:   return "sta";
        case WIFI_CORE_MODE_AP:    return "ap";
        case WIFI_CORE_MODE_APSTA: return "apsta";
        default:                   return "off";
    }
}

esp_err_t wifi_core_start(void)
{
    esp_err_t err = wifi_core_ensure_bootstrap();
    if (err != ESP_OK) return err;

    LOCK();
    wifi_core_mode_t mode = s_st.mode;
    bool sta_configured = s_st.sta_configured;
    wifi_config_t sta_cfg = s_st.sta_cfg;
    bool ap_configured = s_st.ap_configured;
    wifi_config_t ap_cfg = s_st.ap_cfg;
    UNLOCK();

    if (mode == WIFI_CORE_MODE_OFF) {
        ESP_LOGW(TAG, "wifi_core_start: mode is \"off\", call wifi.mode() first");
        return ESP_ERR_INVALID_STATE;
    }

    if ((mode == WIFI_CORE_MODE_STA || mode == WIFI_CORE_MODE_APSTA) && s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if ((mode == WIFI_CORE_MODE_AP || mode == WIFI_CORE_MODE_APSTA) && s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(core_mode_to_esp(mode)));

    if (sta_configured && (mode == WIFI_CORE_MODE_STA || mode == WIFI_CORE_MODE_APSTA)) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }
    if (ap_configured && (mode == WIFI_CORE_MODE_AP || mode == WIFI_CORE_MODE_APSTA)) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    LOCK();
    s_st.started = true;
    UNLOCK();

    if (sta_configured && (mode == WIFI_CORE_MODE_STA || mode == WIFI_CORE_MODE_APSTA)) {
        esp_err_t cerr = esp_wifi_connect();
        if (cerr != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(cerr));
        }
    }

    return ESP_OK;
}

esp_err_t wifi_core_stop(void)
{
    if (!s_bootstrapped) return ESP_OK;

    LOCK();
    bool was_started = s_st.started;
    UNLOCK();
    if (!was_started) return ESP_OK;

    wifi_core_dhcp_stop();

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
    }

    err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
        return err;
    }

    LOCK();
    s_st.started = false;
    s_st.sta_connected = false;
    s_st.sta_has_ip = false;
    s_st.ap_started = false;
    s_st.ap_client_count = 0;
    UNLOCK();

    if (s_sta_netif) { esp_netif_destroy_default_wifi(s_sta_netif); s_sta_netif = NULL; }
    if (s_ap_netif)  { esp_netif_destroy_default_wifi(s_ap_netif);  s_ap_netif = NULL; }
    s_sharing = false;

    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

bool wifi_core_is_started(void)
{
    if (!s_bootstrapped) return false;
    LOCK();
    bool s = s_st.started;
    UNLOCK();
    return s;
}

/* ---------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------- */

wifi_core_status_t wifi_core_get_status(void)
{
    wifi_core_status_t out;
    memset(&out, 0, sizeof(out));
    if (!s_bootstrapped) {
        strcpy(out.sta.auth, "");
        return out;
    }

    LOCK();
    out.mode = s_st.mode;
    out.started = s_st.started;
    UNLOCK();

    /* sta/ap get_info() take the mutex themselves -- call them
     * un-nested to avoid deadlocking on this non-recursive mutex. */
    out.sta = wifi_core_sta_get_info();
    out.ap = wifi_core_ap_get_info();
    return out;
}

/* ---------------------------------------------------------------------
 * Scan
 *
 * esp_wifi_scan_start(cfg, true) is a *blocking ESP-IDF call*: the
 * synchronization (event group + timeout) happens inside the Wi-Fi
 * driver itself, not as a Lua-level "while true do end" polling loop.
 * We call it from whichever task invoked wifi.scan() (the Lua task in
 * this project) which will simply block in this C function for the
 * duration of the scan (typically a few hundred ms) -- there is no
 * busy loop anywhere, in C or in Lua.
 * ------------------------------------------------------------------- */

esp_err_t wifi_core_scan(wifi_core_scan_result_t *out, int max, int *out_count)
{
    esp_err_t err = wifi_core_ensure_bootstrap();
    if (err != ESP_OK) return err;

    if (!wifi_core_is_started()) {
        /* esp_wifi_scan_start requires the driver to be started */
        return ESP_ERR_WIFI_NOT_STARTED;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > (uint16_t)max) ap_num = (uint16_t)max;

    wifi_ap_record_t *records = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (records == NULL && ap_num > 0) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t got = ap_num;
    err = esp_wifi_scan_get_ap_records(&got, records);
    if (err != ESP_OK) {
        free(records);
        return err;
    }

    for (int i = 0; i < got; i++) {
        wifi_core_scan_result_t *r = &out[i];
        memset(r, 0, sizeof(*r));
        strncpy(r->ssid, (const char *)records[i].ssid, WIFI_CORE_MAX_SSID_LEN);
        mac_to_str(records[i].bssid, r->bssid);
        r->rssi = records[i].rssi;
        r->channel = records[i].primary;
        strncpy(r->auth, auth_mode_to_str(records[i].authmode), sizeof(r->auth) - 1);
    }

    free(records);
    *out_count = got;
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * STA
 * ------------------------------------------------------------------- */

esp_err_t wifi_core_sta_config(const char *ssid, const char *password)
{
    esp_err_t err = wifi_core_ensure_bootstrap();
    if (err != ESP_OK) return err;

    if (ssid == NULL || strlen(ssid) == 0 || strlen(ssid) > WIFI_CORE_MAX_SSID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t plen = password ? strlen(password) : 0;
    if (plen > WIFI_CORE_MAX_PASS_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (plen > 0 && plen < 8) {
        /* WPA/WPA2-PSK requires an 8-63 char passphrase */
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (plen > 0) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    LOCK();
    s_st.sta_cfg = cfg;
    s_st.sta_configured = true;
    UNLOCK();

    /* If the driver is already running in a STA-capable mode, apply now */
    if (wifi_core_is_started()) {
        wifi_core_mode_t mode;
        LOCK(); mode = s_st.mode; UNLOCK();
        if (mode == WIFI_CORE_MODE_STA || mode == WIFI_CORE_MODE_APSTA) {
            esp_wifi_set_config(WIFI_IF_STA, &cfg);
        }
    }
    return ESP_OK;
}

esp_err_t wifi_core_sta_connect(void)
{
    if (!wifi_core_is_started()) return ESP_ERR_WIFI_NOT_STARTED;
    /* esp_wifi_connect() is asynchronous: it returns immediately and the
     * result arrives later as WIFI_EVENT_STA_CONNECTED / STA_DISCONNECTED,
     * consumed above in wifi_event_handler(). Lua never blocks here. */
    return esp_wifi_connect();
}

esp_err_t wifi_core_sta_disconnect(void)
{
    if (!wifi_core_is_started()) return ESP_ERR_WIFI_NOT_STARTED;
    return esp_wifi_disconnect();
}

wifi_core_sta_info_t wifi_core_sta_get_info(void)
{
    wifi_core_sta_info_t out;
    memset(&out, 0, sizeof(out));

    if (!s_bootstrapped) return out;

    LOCK();
    out.connected = s_st.sta_connected;
    if (s_st.sta_connected) {
        strncpy(out.ssid, (const char *)s_st.sta_apinfo.ssid, WIFI_CORE_MAX_SSID_LEN);
        mac_to_str(s_st.sta_apinfo.bssid, out.bssid);
        out.channel = s_st.sta_apinfo.primary;
        out.rssi = s_st.sta_apinfo.rssi;
        strncpy(out.auth, auth_mode_to_str(s_st.sta_apinfo.authmode), sizeof(out.auth) - 1);
    }
    out.has_ip = s_st.sta_has_ip;
    if (s_st.sta_has_ip) {
        esp_ip4addr_ntoa(&s_st.sta_ip.ip, out.ip, sizeof(out.ip));
        esp_ip4addr_ntoa(&s_st.sta_ip.gw, out.gateway, sizeof(out.gateway));
        esp_ip4addr_ntoa(&s_st.sta_ip.netmask, out.netmask, sizeof(out.netmask));
    }
    UNLOCK();
    return out;
}

/* ---------------------------------------------------------------------
 * AP
 * ------------------------------------------------------------------- */

esp_err_t wifi_core_ap_config(const char *ssid, const char *password,
                               int channel, int max_conn, bool hidden)
{
    esp_err_t err = wifi_core_ensure_bootstrap();
    if (err != ESP_OK) return err;

    if (ssid == NULL || strlen(ssid) == 0 || strlen(ssid) > WIFI_CORE_MAX_SSID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t plen = password ? strlen(password) : 0;
    if (plen > 0 && plen < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel < 0 || channel > 13) return ESP_ERR_INVALID_ARG;
    if (max_conn <= 0 || max_conn > 10) max_conn = 4;

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = strlen(ssid);
    cfg.ap.channel = (uint8_t)channel;
    cfg.ap.max_connection = (uint8_t)max_conn;
    cfg.ap.ssid_hidden = hidden ? 1 : 0;
    if (plen > 0) {
        strncpy((char *)cfg.ap.password, password, sizeof(cfg.ap.password) - 1);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    LOCK();
    s_st.ap_cfg = cfg;
    s_st.ap_configured = true;
    UNLOCK();

    if (wifi_core_is_started()) {
        wifi_core_mode_t mode;
        LOCK(); mode = s_st.mode; UNLOCK();
        if (mode == WIFI_CORE_MODE_AP || mode == WIFI_CORE_MODE_APSTA) {
            esp_wifi_set_config(WIFI_IF_AP, &cfg);
        }
    }
    return ESP_OK;
}

esp_err_t wifi_core_ap_start(void)
{
    /* AP is (re)started as part of wifi_core_start()/mode switching; this
     * entry point covers the case where AP mode is already running and
     * the caller just wants to (re)apply / bring it back up. */
    if (!wifi_core_is_started()) {
        return wifi_core_start();
    }
    return ESP_OK;
}

esp_err_t wifi_core_ap_stop(void)
{
    wifi_core_mode_t mode;
    LOCK(); mode = s_st.mode; UNLOCK();

    if (mode == WIFI_CORE_MODE_AP) {
        return wifi_core_stop();
    }
    if (mode == WIFI_CORE_MODE_APSTA) {
        /* drop to STA-only */
        return wifi_core_set_mode("sta");
    }
    return ESP_OK;
}

wifi_core_ap_info_t wifi_core_ap_get_info(void)
{
    wifi_core_ap_info_t out;
    memset(&out, 0, sizeof(out));
    if (!s_bootstrapped) return out;

    LOCK();
    out.started = s_st.ap_started;
    if (s_st.ap_configured) {
        strncpy(out.ssid, (const char *)s_st.ap_cfg.ap.ssid, WIFI_CORE_MAX_SSID_LEN);
        out.channel = s_st.ap_cfg.ap.channel;
    }
    out.clients = s_st.ap_client_count;
    UNLOCK();

    if (s_ap_netif && out.started) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
            esp_ip4addr_ntoa(&ip.ip, out.ip, sizeof(out.ip));
            esp_ip4addr_ntoa(&ip.netmask, out.netmask, sizeof(out.netmask));
        }
    }
    return out;
}

esp_err_t wifi_core_ap_clients(wifi_core_client_t *out, int max, int *out_count)
{
    *out_count = 0;
    if (!s_bootstrapped || s_ap_netif == NULL) return ESP_ERR_INVALID_STATE;

    wifi_sta_list_t sta_list;
    esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
    if (err != ESP_OK) return err;

    int n = sta_list.num;
    if (n > max) n = max;

    /* Resolve IPs for as many stations as possible via the DHCP server's
     * ARP-table-backed lookup (esp_netif_dhcps_get_clients_by_mac). */
    esp_netif_pair_mac_ip_t pairs[WIFI_CORE_MAX_CLIENTS];
    memset(pairs, 0, sizeof(pairs));
    for (int i = 0; i < n; i++) {
        memcpy(pairs[i].mac, sta_list.sta[i].mac, 6);
    }
    esp_netif_dhcps_get_clients_by_mac(s_ap_netif, n, pairs);

    for (int i = 0; i < n; i++) {
        wifi_core_client_t *c = &out[i];
        memset(c, 0, sizeof(*c));
        mac_to_str(sta_list.sta[i].mac, c->mac);
        c->rssi = sta_list.sta[i].rssi;
        if (pairs[i].ip.addr != 0) {
            esp_ip4addr_ntoa(&pairs[i].ip, c->ip, sizeof(c->ip));
        }
    }
    *out_count = n;
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * DHCP server (esp_netif's built-in lwIP dhcps, running on the AP netif)
 * ------------------------------------------------------------------- */

static wifi_core_dhcp_config_t s_dhcp_cfg;
static bool s_dhcp_cfg_set = false;

esp_err_t wifi_core_dhcp_config(const wifi_core_dhcp_config_t *cfg)
{
    esp_err_t err = wifi_core_ensure_bootstrap();
    if (err != ESP_OK) return err;

    s_dhcp_cfg = *cfg;
    s_dhcp_cfg_set = true;

    if (s_ap_netif == NULL) {
        /* Stored for when the AP netif is created by wifi_core_start().
         * Applied lazily in wifi_core_dhcp_start(). */
        return ESP_OK;
    }

    bool was_running = wifi_core_dhcp_is_running();
    if (was_running) {
        esp_netif_dhcps_stop(s_ap_netif);
    }

    if (cfg->ip[0] != '\0') {
        esp_netif_ip_info_t ipinfo;
        memset(&ipinfo, 0, sizeof(ipinfo));
        ipinfo.ip.addr = esp_ip4addr_aton(cfg->ip);
        ipinfo.netmask.addr = cfg->netmask[0] ? esp_ip4addr_aton(cfg->netmask)
                                               : esp_ip4addr_aton("255.255.255.0");
        ipinfo.gw.addr = cfg->gateway[0] ? esp_ip4addr_aton(cfg->gateway) : ipinfo.ip.addr;
        esp_err_t iperr = esp_netif_set_ip_info(s_ap_netif, &ipinfo);
        if (iperr != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_set_ip_info (AP) failed: %s", esp_err_to_name(iperr));
        }
    }

    dhcps_lease_t lease;
    memset(&lease, 0, sizeof(lease));
    lease.enable = true;
    lease.start_ip.addr = esp_ip4addr_aton(cfg->start_ip);
    lease.end_ip.addr = esp_ip4addr_aton(cfg->end_ip);

    err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                  ESP_NETIF_REQUESTED_IP_ADDRESS,
                                  &lease, sizeof(lease));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcps set lease range failed: %s", esp_err_to_name(err));
    }

    if (cfg->lease_time_min > 0) {
        uint32_t lt = cfg->lease_time_min;
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                                &lt, sizeof(lt));
    }

    if (was_running) {
        esp_netif_dhcps_start(s_ap_netif);
    }
    return err;
}

esp_err_t wifi_core_dhcp_start(void)
{
    if (s_ap_netif == NULL) {
        ESP_LOGW(TAG, "dhcp.start(): AP netif not up yet -- start AP mode first");
        return ESP_ERR_INVALID_STATE;
    }

    /* esp_netif auto-starts dhcps when the AP netif comes up; this call
     * is for the case wifi.dhcp.stop() was used earlier and the caller
     * wants it back, or for applying a config set before wifi.start(). */
    if (s_dhcp_cfg_set) {
        wifi_core_dhcp_config(&s_dhcp_cfg);
    }

    esp_err_t err = esp_netif_dhcps_start(s_ap_netif);
    if (err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        LOCK(); s_st.dhcps_running = true; UNLOCK();
        return ESP_OK;
    }
    ESP_LOGW(TAG, "esp_netif_dhcps_start: %s", esp_err_to_name(err));
    return err;
}

esp_err_t wifi_core_dhcp_stop(void)
{
    if (s_ap_netif == NULL) return ESP_OK;
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    LOCK(); s_st.dhcps_running = false; UNLOCK();
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return ESP_OK;
    return err;
}

bool wifi_core_dhcp_is_running(void)
{
    if (s_ap_netif) {
        esp_netif_dhcp_status_t status;
        if (esp_netif_dhcps_get_status(s_ap_netif, &status) == ESP_OK) {
            return status == ESP_NETIF_DHCP_STARTED;
        }
    }
    LOCK(); bool r = s_st.dhcps_running; UNLOCK();
    return r;
}

esp_err_t wifi_core_dhcp_leases(wifi_core_lease_t *out, int max, int *out_count)
{
    /* ESP-IDF/lwIP do not expose a direct "list current DHCP leases"
     * call. The documented, supported way to get MAC<->IP pairs for
     * connected AP stations is esp_netif_dhcps_get_clients_by_mac(),
     * which we already use in wifi_core_ap_clients(). We reuse it here;
     * hostname/expiry are not available through the public API, so
     * those fields are intentionally left out rather than guessed. */
    wifi_core_client_t clients[WIFI_CORE_MAX_CLIENTS];
    int n = 0;
    esp_err_t err = wifi_core_ap_clients(clients, WIFI_CORE_MAX_CLIENTS, &n);
    if (err != ESP_OK) { *out_count = 0; return err; }

    if (n > max) n = max;
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (clients[i].ip[0] == '\0') continue; /* no lease resolved yet */
        strncpy(out[count].mac, clients[i].mac, sizeof(out[count].mac) - 1);
        strncpy(out[count].ip, clients[i].ip, sizeof(out[count].ip) - 1);
        count++;
    }
    *out_count = count;
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Network interfaces
 * ------------------------------------------------------------------- */

static void fill_netif_info(esp_netif_t *netif, const char *name, wifi_core_netif_info_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, name, sizeof(out->name) - 1);
    out->up = esp_netif_is_netif_up(netif);
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, out->ip, sizeof(out->ip));
        esp_ip4addr_ntoa(&ip.netmask, out->netmask, sizeof(out->netmask));
        esp_ip4addr_ntoa(&ip.gw, out->gateway, sizeof(out->gateway));
    }
}

int wifi_core_net_interfaces(wifi_core_netif_info_t *out, int max)
{
    int n = 0;
    if (s_sta_netif && n < max) fill_netif_info(s_sta_netif, "sta", &out[n++]);
    if (s_ap_netif && n < max) fill_netif_info(s_ap_netif, "ap", &out[n++]);
    return n;
}

esp_err_t wifi_core_net_info(const char *name, wifi_core_netif_info_t *out)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    if (strcmp(name, "sta") == 0 && s_sta_netif) {
        fill_netif_info(s_sta_netif, "sta", out);
        return ESP_OK;
    }
    if (strcmp(name, "ap") == 0 && s_ap_netif) {
        fill_netif_info(s_ap_netif, "ap", out);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_core_net_dns(const char *iface, char out_ip[16])
{
    out_ip[0] = '\0';
    esp_netif_t *netif = NULL;
    if (iface && strcmp(iface, "sta") == 0) netif = s_sta_netif;
    else if (iface && strcmp(iface, "ap") == 0) netif = s_ap_netif;
    if (netif == NULL) return ESP_ERR_INVALID_STATE;

    esp_netif_dns_info_t dns;
    esp_err_t err = esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) return err;
    esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, out_ip, 16);
    return ESP_OK;
}

const char *wifi_core_net_default_interface(void)
{
    esp_netif_t *def = esp_netif_get_default_netif();
    if (def == NULL) return "none";
    if (def == s_sta_netif) return "sta";
    if (def == s_ap_netif) return "ap";
    return "unknown";
}

/* ---------------------------------------------------------------------
 * Interface-bound request probe
 *
 * ESP-IDF/lwIP does NOT implement SO_BINDTODEVICE. The documented,
 * working alternative is to bind() the socket's local address to the
 * chosen interface's own IP before connect(): lwIP's ip4 routing
 * (ip4_route_src) prefers the netif owning an explicitly bound source
 * address, so this reliably selects the egress interface for
 * same-subnet / default-route destinations. It is NOT a guarantee for
 * an arbitrary destination reachable only via the *other* interface,
 * because the ESP32 SoftAP does not run as a router/NAT gateway unless
 * esp_netif_napt is separately enabled -- this function does not
 * attempt that. See wifi.net.route() to find out which interface is
 * currently the default outbound route.
 * ------------------------------------------------------------------- */

esp_err_t wifi_core_net_request(const char *iface, const char *host, int port,
                                 int timeout_ms, wifi_core_request_result_t *out)
{
    memset(out, 0, sizeof(*out));

    esp_netif_t *netif = NULL;
    if (iface) {
        if (strcmp(iface, "sta") == 0) netif = s_sta_netif;
        else if (strcmp(iface, "ap") == 0) netif = s_ap_netif;
        if (netif == NULL) {
            snprintf(out->error, sizeof(out->error), "interface \"%s\" is not up", iface);
            return ESP_ERR_INVALID_STATE;
        }
    }

    struct sockaddr_in dest = { 0 };
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)port);

    struct in_addr direct;
    if (inet_aton(host, &direct)) {
        dest.sin_addr = direct;
    } else {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        int gerr = getaddrinfo(host, NULL, &hints, &res);
        if (gerr != 0 || res == NULL) {
            snprintf(out->error, sizeof(out->error), "DNS resolution failed for \"%s\"", host);
            return ESP_FAIL;
        }
        dest.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        snprintf(out->error, sizeof(out->error), "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            struct sockaddr_in local = { 0 };
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = ip.ip.addr;
            if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
                snprintf(out->error, sizeof(out->error), "bind() to %s failed: errno %d", iface, errno);
                close(sock);
                return ESP_FAIL;
            }
            esp_ip4addr_ntoa(&ip.ip, out->local_ip, sizeof(out->local_ip));
        }
    }

    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int cerr = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
    if (cerr != 0) {
        snprintf(out->error, sizeof(out->error), "connect() failed: errno %d", errno);
        close(sock);
        return ESP_FAIL;
    }

    close(sock);
    out->success = true;
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Internet sharing (NAT / NAPT)
 * ------------------------------------------------------------------- */

esp_err_t wifi_core_net_share(bool enable)
{
    if (!enable) {
        if (s_ap_netif) {
            esp_netif_napt_disable(s_ap_netif);
        }
        s_sharing = false;
        ESP_LOGI(TAG, "internet sharing disabled");
        return ESP_OK;
    }

    wifi_core_mode_t mode;
    LOCK(); mode = s_st.mode; UNLOCK();
    if (mode != WIFI_CORE_MODE_APSTA || s_sta_netif == NULL || s_ap_netif == NULL) {
        ESP_LOGW(TAG, "wifi.net.share needs mode \"apsta\" with both STA and AP started");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_core_sta_info_t sta = wifi_core_sta_get_info();
    if (!sta.has_ip) {
        ESP_LOGW(TAG, "wifi.net.share: STA has no IP yet -- wait for it to connect first");
        return ESP_ERR_INVALID_STATE;
    }

    /* AP clients' traffic needs an outbound route -- make STA that route
     * before turning NAT on. */
    esp_err_t err = esp_netif_set_default_netif(s_sta_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_set_default_netif(sta) failed: %s", esp_err_to_name(err));
    }

    err = esp_netif_napt_enable(s_ap_netif);
    if (err != ESP_OK) {
        /* Most common cause: CONFIG_LWIP_IP_FORWARD/CONFIG_LWIP_IPV4_NAPT
         * not enabled in sdkconfig, or NAPT already enabled elsewhere
         * (ESP-IDF only supports one interface at a time). */
        ESP_LOGE(TAG, "esp_netif_napt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sharing = true;
    ESP_LOGI(TAG, "internet sharing enabled: AP clients -> NAT -> STA");
    return ESP_OK;
}

bool wifi_core_net_is_sharing(void)
{
    return s_sharing;
}

/* ---------------------------------------------------------------------
 * Signal
 * ------------------------------------------------------------------- */

int wifi_core_signal_quality(int8_t rssi)
{
    /* Normalized 0-100 scale, NOT an ESP-IDF-reported value. Same
     * linear mapping commonly used for Wi-Fi RSSI-to-quality display:
     * -50 dBm or better -> 100%, -100 dBm or worse -> 0%. */
    if (rssi >= -50) return 100;
    if (rssi <= -100) return 0;
    return 2 * (rssi + 100);
}
