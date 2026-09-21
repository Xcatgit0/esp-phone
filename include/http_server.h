#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * http_server - native ESP-IDF (esp_http_server) HTTP server
 *
 * Responsibilities handled internally by this module:
 *   - Mounts the SPIFFS partition labeled "storage" at "/fs"
 *   - Starts esp_http_server (it manages its own FreeRTOS task, so this
 *     module does NOT spin up an extra task for the server loop)
 *   - Serves "GET /" -> "/fs/index.html"
 *   - Serves "GET /fs/ *" as static files straight out of SPIFFS
 *   - Serves "GET /api/status" -> storage size/free info
 *   - Serves "POST /api/ota/storage" -> streaming raw write of the
 *     "storage" partition (SPIFFS image), 8 KB at a time, no full-file
 *     buffering
 *
 * Call http_server_start() once (e.g. after Wi-Fi/network is up).
 * Call http_server_add_get()/http_server_add_post() AFTER
 * http_server_start() to register your own additional endpoints.
 */

/**
 * @brief Mount storage, start esp_http_server and register the built-in
 *        endpoints ("/", "/fs/ *", "/api/status", "/api/ota/storage").
 *
 * @return ESP_OK on success.
 */
esp_err_t http_server_start(void);

/**
 * @brief Stop the HTTP server and unmount the "storage" SPIFFS partition.
 */
esp_err_t http_server_stop(void);

/**
 * @brief Register an additional GET endpoint.
 *
 * Must be called after http_server_start() (the underlying httpd handle
 * must already exist). The uri string is copied internally.
 *
 * @param uri     URI path, e.g. "/api/example"
 * @param handler esp_http_server-style request handler
 */
esp_err_t http_server_add_get(const char *uri,
                               esp_err_t (*handler)(httpd_req_t *req));

/**
 * @brief Register an additional POST endpoint. Same usage as
 *        http_server_add_get().
 */
esp_err_t http_server_add_post(const char *uri,
                                esp_err_t (*handler)(httpd_req_t *req));

#ifdef __cplusplus
}
#endif