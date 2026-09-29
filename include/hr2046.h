#ifndef HR2046_H
#define HR2046_H

#include <stdint.h>
#include "esp_err.h"

/**
 * Initialize HR2046 SPI bus and device.
 */
esp_err_t hr2046_init(void);

/**
 * Read raw 12-bit X ADC value.
 */
uint16_t hr2046_read_x(void);

/**
 * Read raw 12-bit Y ADC value.
 */
uint16_t hr2046_read_y(void);

/**
 * Read raw 12-bit Z1 ADC value.
 */
uint16_t hr2046_read_z1(void);

/**
 * Read raw 12-bit Z2 ADC value.
 */
uint16_t hr2046_read_z2(void);

/**
 * Read all raw values at once.
 */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t z1;
    uint16_t z2;
} hr2046_raw_t;

hr2046_raw_t hr2046_read(void);

/**
 * Register Lua global:
 *
 * touch.raw()
 *
 * Returns:
 * x, y, z1, z2
 */
struct lua_State;

void lua_register_touch(struct lua_State *L);

#endif /* HR2046_H */