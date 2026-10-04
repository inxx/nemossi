idf_component_register(
    SRCS "app_main.c" "board_display.c" "board_input.c" "device_services.c"
         "../model/face.c"
    INCLUDE_DIRS "." "../model"
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc
             esp_driver_i2c esp_timer
)
