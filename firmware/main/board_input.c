#include "board_input.h"
#include "board_pins.h"

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "nm_input";
static QueueHandle_t input_events;
static button_handle_t plus_button;

static void send_edge(nm_input_source_t source, bool pressed)
{
    const nm_input_event_t event = { .source = source, .pressed = pressed };
    if (xQueueSend(input_events, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Input queue full; edge dropped (face has an input timeout)");
    }
}

static void plus_down(void *button, void *context)
{
    (void)button;
    (void)context;
    send_edge(NM_INPUT_PLUS, true);
}

static void plus_up(void *button, void *context)
{
    (void)button;
    (void)context;
    send_edge(NM_INPUT_PLUS, false);
}

#ifdef CONFIG_NEMOSSI_TOUCH_ENABLED
static esp_lcd_touch_handle_t touch;

static void touch_task(void *context)
{
    (void)context;
    bool was_pressed = false;
    bool reported_error = false;
    TickType_t next_poll = xTaskGetTickCount();
    for (;;) {
        esp_err_t error = esp_lcd_touch_read_data(touch);
        if (error == ESP_OK) {
            uint16_t x, y;
            uint8_t points = 0;
            bool pressed = esp_lcd_touch_get_coordinates(touch, &x, &y, NULL,
                                                          &points, 1) && points > 0;
            if (pressed != was_pressed) send_edge(NM_INPUT_TOUCH, pressed);
            was_pressed = pressed;
            reported_error = false;
        } else if (!reported_error) {
            ESP_LOGW(TAG, "Touch read failed: %s", esp_err_to_name(error));
            reported_error = true;
            if (was_pressed) send_edge(NM_INPUT_TOUCH, false);
            was_pressed = false;
        }
        /* I2C reads run separately from rendering and audio/transport adapters. */
        vTaskDelayUntil(&next_poll, pdMS_TO_TICKS(20));
    }
}

static esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = NM_I2C_PORT,
        .sda_io_num = NM_I2C_SDA,
        .scl_io_num = NM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t error = i2c_new_master_bus(&bus_config, &bus);
    if (error != ESP_OK) return error;

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_config.scl_speed_hz = NM_I2C_HZ;
    esp_lcd_panel_io_handle_t io = NULL;
    error = esp_lcd_new_panel_io_i2c(bus, &io_config, &io);
    if (error != ESP_OK) {
        i2c_del_master_bus(bus);
        return error;
    }
    const esp_lcd_touch_config_t touch_config = {
        .x_max = NM_LCD_WIDTH,
        .y_max = NM_LCD_HEIGHT,
        .rst_gpio_num = NM_TOUCH_RESET,
        .int_gpio_num = NM_TOUCH_INTERRUPT,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    error = esp_lcd_touch_new_i2c_cst816s(io, &touch_config, &touch);
    if (error != ESP_OK) {
        esp_lcd_panel_io_del(io);
        i2c_del_master_bus(bus);
        return error;
    }
    if (xTaskCreate(touch_task, "nm_touch", 4096, NULL, 3, NULL) != pdPASS) {
        esp_lcd_touch_del(touch);
        touch = NULL;
        esp_lcd_panel_io_del(io);
        i2c_del_master_bus(bus);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#endif

esp_err_t nm_input_init(QueueHandle_t events)
{
    if (!events) return ESP_ERR_INVALID_ARG;
    input_events = events;
    const button_config_t config = { 0 };
    const button_gpio_config_t gpio = {
        .gpio_num = NM_BUTTON_PLUS,
        .active_level = NM_BUTTON_ACTIVE_LEVEL,
    };
    esp_err_t error = iot_button_new_gpio_device(&config, &gpio, &plus_button);
    if (error != ESP_OK) return error;
    error = iot_button_register_cb(plus_button, BUTTON_PRESS_DOWN, NULL, plus_down, NULL);
    if (error == ESP_OK) {
        error = iot_button_register_cb(plus_button, BUTTON_PRESS_UP, NULL, plus_up, NULL);
    }
    if (error != ESP_OK) {
        iot_button_delete(plus_button);
        plus_button = NULL;
        return error;
    }
#ifdef CONFIG_NEMOSSI_TOUCH_ENABLED
    error = touch_init();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Touch unavailable (%s); PLUS remains enabled", esp_err_to_name(error));
    }
#endif
    return ESP_OK;
}
