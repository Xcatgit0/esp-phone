#pragma once
#include "esp_err.h"
#include "esp_system.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_wifi_set_max_tx_power(int8_t power);
#ifdef __cplusplus
}
#endif
