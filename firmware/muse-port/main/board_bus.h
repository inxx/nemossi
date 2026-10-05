#ifndef NEMOSSI_BOARD_BUS_H
#define NEMOSSI_BOARD_BUS_H

#include "driver/i2c_master.h"
#include "esp_err.h"

/* Initialize from app_main before starting peripheral tasks. Touch and codecs
 * share the same board I2C bus; a peripheral must not delete that shared bus.
 */
esp_err_t nm_i2c_bus_get(i2c_master_bus_handle_t *bus);

#endif
