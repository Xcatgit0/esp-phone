/* ESP-IDF shim: esp_err.h */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int esp_err_t;
#define ESP_OK                    0
#define ESP_FAIL                  (-1)
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_WIFI_BASE         0x3000
#define ESP_ERR_WIFI_NOT_STARTED  (ESP_ERR_WIFI_BASE + 3)
#define ESP_ERR_WIFI_NOT_CONNECT  (ESP_ERR_WIFI_BASE + 10)
const char *esp_err_to_name(esp_err_t code);
#ifdef __cplusplus
}
#endif
#define ESP_ERROR_CHECK(x) do { esp_err_t _rc = (x); if (_rc != ESP_OK) { \
    fprintf(stderr, "ESP_ERROR_CHECK failed: %s (0x%x) at %s:%d\n", esp_err_to_name(_rc), _rc, __FILE__, __LINE__); \
    abort(); } } while (0)
