#include "board_bus.h"
#include "board_pins.h"

esp_err_t nm_i2c_bus_get(i2c_master_bus_handle_t *bus)
{
    if (!bus) return ESP_ERR_INVALID_ARG;
    esp_err_t error = i2c_master_get_bus_handle(NM_I2C_PORT, bus);
    if (error == ESP_OK) return ESP_OK;
    const i2c_master_bus_config_t config = {
        .i2c_port = NM_I2C_PORT,
        .sda_io_num = NM_I2C_SDA,
        .scl_io_num = NM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, bus);
}
