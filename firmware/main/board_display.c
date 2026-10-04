#include "board_display.h"
#include "board_pins.h"

#include <stdatomic.h>
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "sdkconfig.h"

static esp_lcd_panel_handle_t panel;
static uint16_t *frame;
static atomic_bool frame_in_flight = ATOMIC_VAR_INIT(false);

static bool frame_done(esp_lcd_panel_io_handle_t io,
                       esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io;
    (void)event;
    (void)context;
    atomic_store_explicit(&frame_in_flight, false, memory_order_release);
    return false;
}

esp_err_t nm_display_init(void)
{
    const size_t frame_bytes = NM_LCD_WIDTH * NM_LCD_HEIGHT * sizeof(*frame);
    /* Internal DMA memory keeps ownership simple; do not edit until callback.
     * PSRAM is available for future audio queues, not required by this frame.
     */
    frame = heap_caps_malloc(frame_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!frame) return ESP_ERR_NO_MEM;

    const spi_bus_config_t bus = {
        .mosi_io_num = NM_LCD_MOSI,
        .miso_io_num = NM_LCD_MISO,
        .sclk_io_num = NM_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)frame_bytes,
    };
    esp_err_t error = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) return error;

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = NM_LCD_CS,
        .dc_gpio_num = NM_LCD_DC,
        .spi_mode = NM_LCD_SPI_MODE,
        .pclk_hz = NM_LCD_SPI_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = frame_done,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    error = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                    &io_config, &io);
    if (error != ESP_OK) return error;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = NM_LCD_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    error = esp_lcd_new_panel_st7789(io, &panel_config, &panel);
    if (error != ESP_OK) return error;
    error = esp_lcd_panel_reset(panel);
    if (error != ESP_OK) return error;
    error = esp_lcd_panel_init(panel);
    if (error != ESP_OK) return error;
    error = esp_lcd_panel_invert_color(panel, true);
    if (error != ESP_OK) return error;
    error = esp_lcd_panel_disp_on_off(panel, true);
    if (error != ESP_OK) return error;

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    error = ledc_timer_config(&timer);
    if (error != ESP_OK) return error;
    const ledc_channel_config_t backlight = {
        .gpio_num = NM_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = (1023u * CONFIG_NEMOSSI_BACKLIGHT_PERCENT) / 100u,
        .hpoint = 0,
    };
    return ledc_channel_config(&backlight);
}

esp_err_t nm_display_try_present(const face_model_t *face, bool *submitted)
{
    if (submitted) *submitted = false;
    if (!face || !submitted) return ESP_ERR_INVALID_ARG;
    if (!panel || !frame) return ESP_ERR_INVALID_STATE;
    if (atomic_exchange_explicit(&frame_in_flight, true, memory_order_acquire)) {
        return ESP_OK;
    }
    if (!face_render_rgb565(face, frame, NM_LCD_WIDTH * NM_LCD_HEIGHT,
                            NM_LCD_WIDTH, NM_LCD_HEIGHT)) {
        atomic_store_explicit(&frame_in_flight, false, memory_order_release);
        return ESP_ERR_INVALID_ARG;
    }
    /* The ST7789 wire format sends each RGB565 pixel's high byte first. */
    for (size_t i = 0; i < NM_LCD_WIDTH * NM_LCD_HEIGHT; ++i) {
        frame[i] = (uint16_t)((frame[i] >> 8) | (frame[i] << 8));
    }
    esp_err_t error = esp_lcd_panel_draw_bitmap(panel, 0, 0,
                                                NM_LCD_WIDTH, NM_LCD_HEIGHT, frame);
    if (error != ESP_OK) {
        atomic_store_explicit(&frame_in_flight, false, memory_order_release);
        return error;
    }
    *submitted = true;
    return ESP_OK;
}
