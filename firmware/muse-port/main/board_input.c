#include "board_input.h"
#include "board_bus.h"
#include "board_pins.h"

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "link.nemossi_input";
static QueueHandle_t input_events;

static void send_edge(nm_input_source_t source, bool pressed)
{
    const nm_input_event_t event = { .source = source, .pressed = pressed };
    if (xQueueSend(input_events, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Input queue full; edge dropped (face has an input timeout)");
    }
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
    esp_err_t error = nm_i2c_bus_get(&bus);
    if (error != ESP_OK) return error;

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_config.scl_speed_hz = NM_I2C_HZ;
    esp_lcd_panel_io_handle_t io = NULL;
    error = esp_lcd_new_panel_io_i2c(bus, &io_config, &io);
    if (error != ESP_OK) {
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
        return error;
    }
    if (xTaskCreate(touch_task, "nm_touch", 4096, NULL, 3, NULL) != pdPASS) {
        esp_lcd_touch_del(touch);
        touch = NULL;
        esp_lcd_panel_io_del(io);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#endif

esp_err_t nm_input_init(QueueHandle_t events)
{
    if (!events) return ESP_ERR_INVALID_ARG;
    input_events = events;
#ifdef CONFIG_NEMOSSI_TOUCH_ENABLED
    esp_err_t error = touch_init();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Touch unavailable (%s); default face retained", esp_err_to_name(error));
    }
#endif
    return ESP_OK;
}
