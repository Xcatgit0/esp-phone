#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_crt_bundle_attach(void *conf);   /* emulator: no-op, libcurl uses the host's CA store */
#ifdef __cplusplus
}
#endif
