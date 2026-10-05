/* SPDX-License-Identifier: Apache-2.0
 * Host-only peripheral fakes. These do not establish hardware compatibility.
 */
#ifndef NEMOSSI_VOICE_BOARD_HOST_H
#define NEMOSSI_VOICE_BOARD_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "board_pins.h"
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
       ESP_ERR_INVALID_SIZE, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT };
enum { ESP_CODEC_DEV_OK, ESP_CODEC_DEV_INVALID_ARG = -1,
       ESP_CODEC_DEV_READ_FAIL = -2, ESP_CODEC_DEV_WRITE_FAIL = -3,
       ESP_CODEC_DEV_DRV_ERR = -4 };
#define VOICE_MIC_RATE 16000
#define VOICE_SPEAKER_RATE 48000
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
const char *esp_err_to_name(esp_err_t error);
void nm_face_speaker_pcm(const int16_t *pcm, size_t samples);
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffu
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout);
int xSemaphoreGive(SemaphoreHandle_t lock);
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
#define GPIO_MODE_OUTPUT 1
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(int pin, unsigned level);
typedef void *i2c_master_bus_handle_t;
typedef struct fake_i2c *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length; unsigned device_address; int scl_speed_hz; } i2c_device_config_t;
#define I2C_ADDR_BIT_LEN_7 7
esp_err_t nm_i2c_bus_get(i2c_master_bus_handle_t *bus);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
    const i2c_device_config_t *config, i2c_master_dev_handle_t *device);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
    const uint8_t *out, size_t out_size, uint8_t *in, size_t in_size, int timeout);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
    const uint8_t *out, size_t size, int timeout);

typedef struct fake_i2s *i2s_chan_handle_t;
typedef struct { int id, role, dma_desc_num, dma_frame_num; bool auto_clear_after_cb; } i2s_chan_config_t;
typedef struct { int sample_rate_hz, mclk_multiple; } fake_clock_t;
typedef struct {
    int data_bit_width, slot_mode, slot_bit_width, ws_width, slot_mask, total_slot;
    bool left_align;
} fake_slot_t;
typedef struct { int mclk, bclk, ws, dout, din; } fake_gpio_t;
typedef struct { fake_clock_t clk_cfg; fake_slot_t slot_cfg; fake_gpio_t gpio_cfg; } i2s_std_config_t;
typedef i2s_std_config_t i2s_tdm_config_t;
#define I2S_ROLE_MASTER 1
#define I2S_GPIO_UNUSED (-1)
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_BIT_WIDTH_32BIT 32
#define I2S_SLOT_MODE_STEREO 2
#define I2S_TDM_SLOT0 1
#define I2S_CHANNEL_DEFAULT_CONFIG(port, clockrole) ((i2s_chan_config_t){.id=(port),.role=(clockrole)})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) ((fake_clock_t){.sample_rate_hz=(rate),.mclk_multiple=256})
#define I2S_TDM_CLK_DEFAULT_CONFIG(rate) I2S_STD_CLK_DEFAULT_CONFIG(rate)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, channels) ((fake_slot_t){.data_bit_width=(bits),.slot_bit_width=(bits),.slot_mode=(channels)})
#define I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(bits, channels, slots) ((fake_slot_t){.data_bit_width=(bits),.slot_bit_width=(bits),.slot_mode=(channels),.slot_mask=(slots)})
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *config);
esp_err_t i2s_channel_init_tdm_mode(i2s_chan_handle_t channel, const i2s_tdm_config_t *config);
esp_err_t i2s_channel_enable(i2s_chan_handle_t channel);
esp_err_t i2s_channel_disable(i2s_chan_handle_t channel);
esp_err_t i2s_del_channel(i2s_chan_handle_t channel);
esp_err_t i2s_channel_read(i2s_chan_handle_t channel, void *data, size_t size, size_t *read, uint32_t timeout);
esp_err_t i2s_channel_write(i2s_chan_handle_t channel, const void *data, size_t size, size_t *written, uint32_t timeout);

typedef struct audio_codec_ctrl_if_t audio_codec_ctrl_if_t;
typedef struct { int type; struct { uint16_t addr; uint8_t port; } i2c; } audio_codec_ctrl_info_t;
#define AUDIO_CODEC_CTRL_I2C 1
struct audio_codec_ctrl_if_t {
    int (*open)(const audio_codec_ctrl_if_t *, void *, int);
    bool (*is_open)(const audio_codec_ctrl_if_t *);
    int (*read_reg)(const audio_codec_ctrl_if_t *, int, int, void *, int);
    int (*write_reg)(const audio_codec_ctrl_if_t *, int, int, void *, int);
    int (*get_info)(const audio_codec_ctrl_if_t *, audio_codec_ctrl_info_t *);
    int (*close)(const audio_codec_ctrl_if_t *);
};
typedef enum { ESP_CODEC_DEV_TYPE_IN = 1, ESP_CODEC_DEV_TYPE_OUT = 2 } esp_codec_dev_type_t;
typedef struct { int sample_rate, channel, channel_mask, bits_per_sample, mclk_multiple; } esp_codec_dev_sample_info_t;
typedef struct audio_codec_data_if_t audio_codec_data_if_t;
struct audio_codec_data_if_t { int (*set_fmt)(const audio_codec_data_if_t *, esp_codec_dev_type_t, esp_codec_dev_sample_info_t *); };
typedef struct { int port; void *tx_handle, *rx_handle; } audio_codec_i2s_cfg_t;
typedef struct { const audio_codec_ctrl_if_t *control; bool output; } audio_codec_if_t;
typedef struct {
    const audio_codec_ctrl_if_t *ctrl_if;
    int codec_mode, pa_pin, mclk_div;
    bool use_mclk;
    struct { float pa_voltage, codec_dac_voltage; } hw_gain;
} es8311_codec_cfg_t;
typedef struct { const audio_codec_ctrl_if_t *ctrl_if; int mic_selected, mclk_src, mclk_div; } es7210_codec_cfg_t;
#define ES8311_CODEC_DEFAULT_ADDR 0x30
#define ES7210_CODEC_DEFAULT_ADDR 0x80
#define ESP_CODEC_DEV_WORK_MODE_DAC 1
#define ES7210_SEL_MIC1 1
#define ES7210_SEL_MIC2 2
#define ES7210_SEL_MIC3 4
#define ES7210_SEL_MIC4 8
#define ES7210_MCLK_FROM_PAD 0
#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel) (1u << (channel))
typedef struct fake_codec_device *esp_codec_dev_handle_t;
typedef struct { esp_codec_dev_type_t dev_type; const audio_codec_if_t *codec_if; const audio_codec_data_if_t *data_if; } esp_codec_dev_cfg_t;
const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *config);
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *config);
const audio_codec_if_t *es7210_codec_new(es7210_codec_cfg_t *config);
esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *config);
int esp_codec_dev_open(esp_codec_dev_handle_t device, esp_codec_dev_sample_info_t *format);
int esp_codec_dev_close(esp_codec_dev_handle_t device);
void esp_codec_dev_delete(esp_codec_dev_handle_t device);
int audio_codec_delete_codec_if(const audio_codec_if_t *codec);
int audio_codec_delete_data_if(const audio_codec_data_if_t *data);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t device, int percent);
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t device, bool mute);
int esp_codec_dev_set_in_mute(esp_codec_dev_handle_t device, bool mute);
int esp_codec_dev_set_in_channel_gain(esp_codec_dev_handle_t device, uint16_t channels, float gain);
#endif
