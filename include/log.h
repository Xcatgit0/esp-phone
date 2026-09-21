#ifndef LOG_H
#define LOG_H
#include <stdio.h>
#include <stdint.h>
#include "esp_timer.h"

#define LOG_INFO(fmt, ...) \
    printf("I (%lld) " fmt "\n", \
           esp_timer_get_time() / 1000, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    printf("W (%lld) " fmt "\n", \
           esp_timer_get_time() / 1000, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    printf("E (%lld) " fmt "\n", \
           esp_timer_get_time() / 1000, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    printf("D (%lld) " fmt "\n", \
           esp_timer_get_time() / 1000, ##__VA_ARGS__)

#endif