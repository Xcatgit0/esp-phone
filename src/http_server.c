#include "http_server.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_spiffs.h"
#include "esp_partition.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "http_server";

#define STORAGE_PARTITION_LABEL "storage"
#define STORAGE_MOUNT_POINT     "/fs"
#define IO_CHUNK_SIZE           8192
#define OTA_RECV_MAX_RETRIES    5

static httpd_handle_t    s_server         = NULL;
static SemaphoreHandle_t s_storage_mutex  = NULL;
static bool               s_spiffs_mounted = false;

/* Single shared fixed-size I/O buffer. Access is always serialized by
 * s_storage_mutex (only one static-file read OR one OTA write happens at
 * a time), so it is safe to share between the two paths and avoids any
 * malloc()/heap use for file/partition I/O. */
static uint8_t s_io_buf[IO_CHUNK_SIZE];

/* ------------------------------------------------------------------- */
/* small helpers                                                        */
/* ------------------------------------------------------------------- */

static void send_json_status(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":false,\"message\":\"%s\"}", msg);
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool path_is_safe(const char *uri)
{
    /* Reject any ".." segment to block path traversal, e.g. /fs/../.. */
    return strstr(uri, "..") == NULL;
}

static const char *content_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".txt")  == 0) return "text/plain";
    return "application/octet-stream";
}

/* ------------------------------------------------------------------- */
/* SPIFFS mount / unmount                                               */
/* ------------------------------------------------------------------- */

static esp_err_t storage_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = STORAGE_MOUNT_POINT,
        .partition_label        = STORAGE_PARTITION_LABEL,
        .max_files               = 16,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        s_spiffs_mounted = false;
        return err;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(STORAGE_PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted (label=\"%s\"): %u/%u bytes used",
             STORAGE_PARTITION_LABEL, (unsigned)used, (unsigned)total);

    s_spiffs_mounted = true;
    return ESP_OK;
}

static esp_err_t storage_unmount(void)
{
    if (!s_spiffs_mounted) {
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_spiffs_unregister(STORAGE_PARTITION_LABEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS unmount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_spiffs_mounted = false;
    return ESP_OK;
}

/* ------------------------------------------------------------------- */
/* static file serving                                                  */
/* ------------------------------------------------------------------- */

/* Caller must hold s_storage_mutex and have verified s_spiffs_mounted. */
static esp_err_t send_file(httpd_req_t *req, const char *fs_path)
{
    FILE *f = fopen(fs_path, "r");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", fs_path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type_for(fs_path));

    esp_err_t ret = ESP_OK;
    size_t read_bytes;
    bool send_failed = false;

    do {
        read_bytes = fread(s_io_buf, 1, sizeof(s_io_buf), f);
        if (read_bytes > 0) {
            if (httpd_resp_send_chunk(req, (const char *)s_io_buf, read_bytes) != ESP_OK) {
                ESP_LOGE(TAG, "Chunk send failed for %s", fs_path);
                ret = ESP_FAIL;
                send_failed = true;
                break;
            }
        }
    } while (read_bytes == sizeof(s_io_buf));

    if (ferror(f)) {
        ESP_LOGE(TAG, "Read error on %s", fs_path);
        ret = ESP_FAIL;
    }

    fclose(f);

    /* Only safe to terminate the chunked response if send_chunk itself
     * did not already fail (a failed send_chunk tears down the
     * connection on its own). */
    if (!send_failed) {
        httpd_resp_send_chunk(req, NULL, 0);
    }

    return ret;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);

    if (!s_spiffs_mounted) {
        xSemaphoreGive(s_storage_mutex);
        send_json_status(req, "503 Service Unavailable", "storage busy");
        return ESP_FAIL;
    }

    esp_err_t ret = send_file(req, STORAGE_MOUNT_POINT "/index.html");
    xSemaphoreGive(s_storage_mutex);
    return ret;
}

static esp_err_t static_file_handler(httpd_req_t *req)
{
    if (!path_is_safe(req->uri)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);

    if (!s_spiffs_mounted) {
        xSemaphoreGive(s_storage_mutex);
        send_json_status(req, "503 Service Unavailable", "storage busy");
        return ESP_FAIL;
    }

    /* req->uri already starts with "/fs/..." which is exactly our SPIFFS
     * mount point, so it maps 1:1 to the file inside the image
     * (e.g. "/fs/index.html" -> the "/index.html" entry in the SPIFFS
     * image). No manual prefix rewriting needed. */
    ESP_LOGI(TAG, "GET %s", req->uri);
    esp_err_t ret = send_file(req, req->uri);

    xSemaphoreGive(s_storage_mutex);
    return ret;
}

/* ------------------------------------------------------------------- */
/* /api/status                                                          */
/* ------------------------------------------------------------------- */

static esp_err_t status_handler(httpd_req_t *req)
{
    size_t total = 0, used = 0;
    bool mounted;

    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    mounted = s_spiffs_mounted;
    if (mounted) {
        esp_spiffs_info(STORAGE_PARTITION_LABEL, &total, &used);
    }
    xSemaphoreGive(s_storage_mutex);

    char body[192];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"storage_mounted\":%s,\"storage_size\":%u,\"storage_free\":%u}",
             mounted ? "true" : "false",
             (unsigned)total, (unsigned)(total - used));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* ------------------------------------------------------------------- */
/* POST /api/ota/storage                                                */
/* ------------------------------------------------------------------- */

static esp_err_t ota_storage_handler(httpd_req_t *req)
{
    if (req->content_len == 0) {
        ESP_LOGW(TAG, "OTA request missing/zero Content-Length");
        send_json_status(req, "411 Length Required", "Content-Length required");
        return ESP_FAIL;
    }

    size_t content_len = req->content_len;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        STORAGE_PARTITION_LABEL);

    if (!part) {
        ESP_LOGE(TAG, "\"%s\" partition not found", STORAGE_PARTITION_LABEL);
        send_json_status(req, "500 Internal Server Error", "storage partition not found");
        return ESP_FAIL;
    }

    if (content_len > part->size) {
        ESP_LOGE(TAG, "OTA payload %u bytes exceeds partition size %u bytes",
                 (unsigned)content_len, (unsigned)part->size);
        send_json_status(req, "413 Payload Too Large", "file exceeds storage partition size");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA size %u bytes (partition size %u bytes)",
             (unsigned)content_len, (unsigned)part->size);

    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);

    /* Unmount SPIFFS first: nothing must read the partition while we
     * erase/rewrite it underneath the filesystem. */
    if (storage_unmount() != ESP_OK) {
        xSemaphoreGive(s_storage_mutex);
        send_json_status(req, "500 Internal Server Error", "failed to unmount storage");
        return ESP_FAIL;
    }

    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
        storage_mount(); /* best-effort: leave the server able to keep serving */
        xSemaphoreGive(s_storage_mutex);
        send_json_status(req, "500 Internal Server Error", "partition erase failed");
        return ESP_FAIL;
    }

    size_t written = 0;
    size_t next_log_mark = content_len / 10;
    if (next_log_mark == 0) {
        next_log_mark = content_len; /* tiny file: just log at 100% */
    }

    while (written < content_len) {
        size_t to_read = MIN((size_t)IO_CHUNK_SIZE, content_len - written);
        int r;
        int retries = 0;

        for (;;) {
            r = httpd_req_recv(req, (char *)s_io_buf, to_read);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                retries++;
                if (retries > OTA_RECV_MAX_RETRIES) {
                    ESP_LOGE(TAG, "recv timed out repeatedly, aborting OTA");
                    r = -1;
                    break;
                }
                ESP_LOGW(TAG, "recv timeout, retry %d/%d", retries, OTA_RECV_MAX_RETRIES);
                continue;
            }
            break;
        }

        if (r <= 0) {
            ESP_LOGE(TAG, "OTA aborted: recv failed at %u/%u bytes",
                     (unsigned)written, (unsigned)content_len);
            storage_mount();
            xSemaphoreGive(s_storage_mutex);
            send_json_status(req, "500 Internal Server Error", "upload interrupted");
            return ESP_FAIL;
        }

        err = esp_partition_write(part, written, s_io_buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "partition write failed at offset %u: %s",
                     (unsigned)written, esp_err_to_name(err));
            storage_mount();
            xSemaphoreGive(s_storage_mutex);
            send_json_status(req, "500 Internal Server Error", "flash write failed");
            return ESP_FAIL;
        }

        written += (size_t)r;

        if (written >= next_log_mark) {
            ESP_LOGI(TAG, "OTA progress: %u/%u bytes (%u%%)",
                     (unsigned)written, (unsigned)content_len,
                     (unsigned)((uint64_t)written * 100 / content_len));
            next_log_mark += content_len / 10;
            if (next_log_mark == 0 || next_log_mark > content_len) {
                next_log_mark = content_len;
            }
        }
    }

    ESP_LOGI(TAG, "OTA write complete: %u bytes written", (unsigned)written);

    err = storage_mount();
    xSemaphoreGive(s_storage_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA data written OK but remount failed: %s", esp_err_to_name(err));
        send_json_status(req, "500 Internal Server Error", "write ok but remount failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA completed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"storage OTA complete\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------- */
/* public API                                                           */
/* ------------------------------------------------------------------- */

esp_err_t http_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "server already running");
        return ESP_OK;
    }

    if (s_storage_mutex == NULL) {
        s_storage_mutex = xSemaphoreCreateMutex();
        if (!s_storage_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = storage_mount();
    if (err != ESP_OK) {
        return err;
    }

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn     = httpd_uri_match_wildcard; /* needed for "/fs/ *" */
    config.stack_size       = 8192;
    config.max_uri_handlers = 16;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    /* esp_http_server spawns and owns its own task internally
     * (httpd_start creates it) - we do not create an additional task
     * for the server loop here. */
    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        storage_unmount();
        return err;
    }

    static const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler,
    };
    static const httpd_uri_t fs_uri = {
        .uri = "/fs/*", .method = HTTP_GET, .handler = static_file_handler,
    };
    static const httpd_uri_t status_uri = {
        .uri = "/api/status", .method = HTTP_GET, .handler = status_handler,
    };
    static const httpd_uri_t ota_uri = {
        .uri = "/api/ota/storage", .method = HTTP_POST, .handler = ota_storage_handler,
    };

    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &fs_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &ota_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    storage_unmount();
    return ESP_OK;
}

esp_err_t http_server_add_get(const char *uri, esp_err_t (*handler)(httpd_req_t *req))
{
    if (!s_server) {
        ESP_LOGE(TAG, "http_server_add_get: server not started yet");
        return ESP_ERR_INVALID_STATE;
    }

    /* esp_http_server keeps a pointer to this struct (and its uri
     * string), so it must stay alive for as long as the handler is
     * registered - we intentionally do not free it on success. */
    httpd_uri_t *u = calloc(1, sizeof(httpd_uri_t));
    if (!u) return ESP_ERR_NO_MEM;

    u->uri     = strdup(uri);
    u->method  = HTTP_GET;
    u->handler = handler;

    esp_err_t err = httpd_register_uri_handler(s_server, u);
    if (err != ESP_OK) {
        free((void *)u->uri);
        free(u);
    } else {
        ESP_LOGI(TAG, "registered GET %s", uri);
    }
    return err;
}

esp_err_t http_server_add_post(const char *uri, esp_err_t (*handler)(httpd_req_t *req))
{
    if (!s_server) {
        ESP_LOGE(TAG, "http_server_add_post: server not started yet");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_uri_t *u = calloc(1, sizeof(httpd_uri_t));
    if (!u) return ESP_ERR_NO_MEM;

    u->uri     = strdup(uri);
    u->method  = HTTP_POST;
    u->handler = handler;

    esp_err_t err = httpd_register_uri_handler(s_server, u);
    if (err != ESP_OK) {
        free((void *)u->uri);
        free(u);
    } else {
        ESP_LOGI(TAG, "registered POST %s", uri);
    }
    return err;
}