/* SPDX-License-Identifier: Apache-2.0
 * Original Nemossi backend for the Muse voice_board interface.
 * Wiring: waveshareteam/ESP32-S3-Touch-LCD-1.54 @
 * 5157db7c888e476fd57f8a95020800377447478f,
 * examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/
 * components/esp_bsp/bsp_codec.c.
 * Public codec interfaces: esp_codec_dev 1.6.2 @
 * af7b72fb2b73d4f513d3cede01c13518ab216735. No vendor code copied.
 * This is a half-duplex 16 kHz port, not XMOS/AEC hardware.
 */
#ifdef NEMOSSI_VOICE_HOST_TEST
#include "voice_board_nemossi_host.h"
#else
#include "voice_board.h"
#include "board_bus.h"
#include "board_pins.h"
#include "nemossi_face.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif
#include <limits.h>
#include <string.h>

#define NM_VOICE_CHUNK 320u
#define NM_VOICE_OUTPUT_CHUNK 32u
#define NM_VOICE_MAX_VOLUME 20
#define NM_CODEC_TIMEOUT_MS 100
#define NM_DAC_MUTE_REG 0x31
#define NM_DAC_VOLUME_REG 0x32
#define NM_ADC_MUTE12_REG 0x14
#define NM_ADC_MUTE34_REG 0x15

static const char *TAG = "link.nemossi_voice";
static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_data_if_t *s_data;
static const audio_codec_if_t *s_dac, *s_adc;
static esp_codec_dev_handle_t s_output, s_input;
static bool s_ready, s_capture, s_amp, s_rx_enabled;
static int s_volume = NM_VOICE_MAX_VOLUME;

/* The library's public volume/mute API ignores some codec return values.
 * Keep every control error sticky, including during codec construction, and
 * read back runtime controls. A later successful transfer cannot hide a fault.
 */
typedef struct {
    audio_codec_ctrl_if_t base;
    i2c_master_dev_handle_t device;
    uint8_t address8;
    bool fault, volume_written;
    uint8_t volume_value;
} nm_control_t;
static nm_control_t s_dac_control, s_adc_control;

static int ctrl_read(const audio_codec_ctrl_if_t *base, int reg, int reg_len,
                     void *data, int length)
{
    nm_control_t *ctrl = (nm_control_t *)base;
    if (!ctrl->device || !data || reg_len != 1 || reg < 0 || reg > 255 || length != 1) {
        ctrl->fault = true;
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    const uint8_t address = (uint8_t)reg;
    const esp_err_t err = i2c_master_transmit_receive(ctrl->device, &address, 1,
        data, 1, NM_CODEC_TIMEOUT_MS);
    if (err != ESP_OK) ctrl->fault = true;
    return err == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_READ_FAIL;
}

static int ctrl_write(const audio_codec_ctrl_if_t *base, int reg, int reg_len,
                      void *data, int length)
{
    nm_control_t *ctrl = (nm_control_t *)base;
    if (!ctrl->device || !data || reg_len != 1 || reg < 0 || reg > 255 || length != 1) {
        ctrl->fault = true;
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    const uint8_t bytes[] = {(uint8_t)reg, *(const uint8_t *)data};
    const esp_err_t err = i2c_master_transmit(ctrl->device, bytes, sizeof(bytes),
                                             NM_CODEC_TIMEOUT_MS);
    if (err != ESP_OK) ctrl->fault = true;
    if (err == ESP_OK && reg == NM_DAC_VOLUME_REG) {
        ctrl->volume_written = true;
        ctrl->volume_value = bytes[1];
    }
    return err == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static bool ctrl_is_open(const audio_codec_ctrl_if_t *base)
{
    return ((const nm_control_t *)base)->device != NULL;
}

static int ctrl_info(const audio_codec_ctrl_if_t *base, audio_codec_ctrl_info_t *info)
{
    if (!info) return ESP_CODEC_DEV_INVALID_ARG;
    info->type = AUDIO_CODEC_CTRL_I2C;
    info->i2c.addr = ((const nm_control_t *)base)->address8;
    info->i2c.port = NM_I2C_PORT;
    return ESP_CODEC_DEV_OK;
}

static int ctrl_close(const audio_codec_ctrl_if_t *base)
{
    nm_control_t *ctrl = (nm_control_t *)base;
    if (ctrl->device) {
        if (i2c_master_bus_rm_device(ctrl->device) != ESP_OK) return ESP_CODEC_DEV_DRV_ERR;
        ctrl->device = NULL;
    }
    return ESP_CODEC_DEV_OK;
}

static esp_err_t ctrl_attach(nm_control_t *ctrl, i2c_master_bus_handle_t bus,
                             uint8_t address8)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->base = (audio_codec_ctrl_if_t) {
        .is_open = ctrl_is_open, .read_reg = ctrl_read, .write_reg = ctrl_write,
        .get_info = ctrl_info, .close = ctrl_close,
    };
    ctrl->address8 = address8;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address8 >> 1,
        .scl_speed_hz = NM_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &config, &ctrl->device);
}

/* Called under s_lock. Leave allocated channels intact on a runtime fault;
 * another worker may still own a transfer. Every public operation fails closed.
 */
static void fail_closed(void)
{
    s_ready = false;
    s_amp = false;
    gpio_set_level(NM_AUDIO_AMP_ENABLE, 0);
}

static bool controls_ok(void)
{
    return !s_dac_control.fault && !s_adc_control.fault;
}

static bool check_register(nm_control_t *ctrl, uint8_t reg, uint8_t mask,
                            uint8_t expected)
{
    uint8_t actual = 0;
    if (ctrl_read(&ctrl->base, reg, 1, &actual, 1) != ESP_CODEC_DEV_OK ||
        (actual & mask) != (expected & mask)) {
        ctrl->fault = true;
        return false;
    }
    return true;
}

static bool output_control(bool mute)
{
    /* No software volume handler is installed: ES8311 writes DAC register32. */
    s_dac_control.volume_written = false;
    if (esp_codec_dev_set_out_vol(s_output, s_volume) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(s_output, mute || s_volume == 0) != ESP_CODEC_DEV_OK ||
        !controls_ok() || !s_dac_control.volume_written ||
        !check_register(&s_dac_control, NM_DAC_VOLUME_REG, 0xff,
                         s_dac_control.volume_value) ||
        !check_register(&s_dac_control, NM_DAC_MUTE_REG, 0x60,
                         mute || s_volume == 0 ? 0x60 : 0)) {
        fail_closed();
        return false;
    }
    return true;
}

static bool input_control(bool mute)
{
    if (esp_codec_dev_set_in_mute(s_input, mute) != ESP_CODEC_DEV_OK ||
        !controls_ok() ||
        !check_register(&s_adc_control, NM_ADC_MUTE12_REG, 3, mute ? 3 : 0) ||
        !check_register(&s_adc_control, NM_ADC_MUTE34_REG, 3, mute ? 3 : 0)) {
        fail_closed();
        return false;
    }
    return true;
}

static void cleanup(void)
{
    fail_closed();
    s_capture = false;
    if (s_input) { esp_codec_dev_close(s_input); esp_codec_dev_delete(s_input); }
    if (s_output) { esp_codec_dev_close(s_output); esp_codec_dev_delete(s_output); }
    s_input = s_output = NULL;
    if (s_adc) audio_codec_delete_codec_if(s_adc);
    if (s_dac) audio_codec_delete_codec_if(s_dac);
    s_adc = s_dac = NULL;
    ctrl_close(&s_adc_control.base);
    ctrl_close(&s_dac_control.base);
    if (s_data) audio_codec_delete_data_if(s_data);
    s_data = NULL;
    if (s_rx) { i2s_channel_disable(s_rx); i2s_del_channel(s_rx); }
    if (s_tx) { i2s_channel_disable(s_tx); i2s_del_channel(s_tx); }
    s_rx = s_tx = NULL;
    s_rx_enabled = false;
    /* The shared touch/codec bus is owned by board_bus, never deleted here. */
}

static esp_err_t configure_i2s(void)
{
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(NM_I2S_PORT, I2S_ROLE_MASTER);
    channels.dma_desc_num = 6;
    channels.dma_frame_num = NM_VOICE_CHUNK;
    channels.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&channels, &s_tx, &s_rx);
    if (err != ESP_OK) return err;
    i2s_std_config_t tx = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = NM_I2S_MCLK, .bclk = NM_I2S_BCLK,
            .ws = NM_I2S_LRCLK, .dout = NM_I2S_DOUT, .din = I2S_GPIO_UNUSED },
    };
    tx.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    tx.slot_cfg.ws_width = 32;
    err = i2s_channel_init_std_mode(s_tx, &tx);
    if (err != ESP_OK) return err;
    i2s_tdm_config_t rx = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO,
                                                       I2S_TDM_SLOT0),
        .gpio_cfg = { .mclk = NM_I2S_MCLK, .bclk = NM_I2S_BCLK,
            .ws = NM_I2S_LRCLK, .dout = I2S_GPIO_UNUSED, .din = NM_I2S_DIN },
    };
    rx.slot_cfg.total_slot = 4;
    rx.slot_cfg.ws_width = 32;
    rx.slot_cfg.left_align = true;
    /* TX=2x32, RX=4x16: 64 BCLK/frame, MCLK=256Fs. Paired I2S0
     * channels make RX a clock slave; keep its SDK slave bclk_div default. */
    return i2s_channel_init_tdm_mode(s_rx, &rx);
}

esp_err_t voice_board_init(void)
{
    /* Called once by the official voice task, before its workers start. */
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ready) { xSemaphoreGive(s_lock); return ESP_OK; }
    cleanup();
    s_volume = NM_VOICE_MAX_VOLUME;
    const gpio_config_t pa = {
        .pin_bit_mask = 1ULL << NM_AUDIO_AMP_ENABLE, .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&pa);
    if (err == ESP_OK) err = gpio_set_level(NM_AUDIO_AMP_ENABLE, 0);
    i2c_master_bus_handle_t bus = NULL;
    if (err == ESP_OK) err = nm_i2c_bus_get(&bus);
    if (err == ESP_OK) err = configure_i2s();
    if (err == ESP_OK) err = ctrl_attach(&s_dac_control, bus, ES8311_CODEC_DEFAULT_ADDR);
    if (err == ESP_OK) err = ctrl_attach(&s_adc_control, bus, ES7210_CODEC_DEFAULT_ADDR);
    if (err != ESP_OK) goto failed;
    audio_codec_i2s_cfg_t data = {.port = NM_I2S_PORT, .tx_handle = s_tx, .rx_handle = s_rx};
    s_data = audio_codec_new_i2s_data(&data);
    es8311_codec_cfg_t dac = {
        .ctrl_if = &s_dac_control.base, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, .use_mclk = true, .mclk_div = 256,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    };
    es7210_codec_cfg_t adc = {
        .ctrl_if = &s_adc_control.base,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
        .mclk_src = ES7210_MCLK_FROM_PAD, .mclk_div = 256,
    };
    s_dac = es8311_codec_new(&dac);
    s_adc = es7210_codec_new(&adc);
    if (!s_data || !s_dac || !s_adc || !controls_ok()) { err = ESP_FAIL; goto failed; }
    esp_codec_dev_cfg_t dev = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = s_dac, .data_if = s_data};
    s_output = esp_codec_dev_new(&dev);
    dev.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev.codec_if = s_adc;
    s_input = esp_codec_dev_new(&dev);
    if (!s_output || !s_input) { err = ESP_ERR_NO_MEM; goto failed; }
    esp_codec_dev_sample_info_t tx = {
        .sample_rate = VOICE_MIC_RATE, .channel = 2, .channel_mask = 3,
        .bits_per_sample = 16, .mclk_multiple = 256,
    };
    esp_codec_dev_sample_info_t rx = {
        .sample_rate = VOICE_MIC_RATE, .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .bits_per_sample = 16, .mclk_multiple = 256,
    };
    /* Pre-check ignored codec data-interface errors; formats stay immutable
     * after init. Only slot0 is transferred, so RX DMA is mono PCM16. */
    if (s_data->set_fmt(s_data, ESP_CODEC_DEV_TYPE_OUT, &tx) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_open(s_output, &tx) != ESP_CODEC_DEV_OK ||
        s_data->set_fmt(s_data, ESP_CODEC_DEV_TYPE_IN, &rx) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_open(s_input, &rx) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_channel_gain(s_input, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 30.0f) != ESP_CODEC_DEV_OK ||
        !controls_ok() || !output_control(true) || !input_control(true)) {
        err = ESP_FAIL; goto failed;
    }
    int16_t unused = 0;
    size_t transferred = 0;
    err = i2s_channel_write(s_tx, &unused, 0, &transferred, 0);
    if (err == ESP_OK) err = i2s_channel_read(s_rx, &unused, 0, &transferred, 0);
    if (err == ESP_OK) err = i2s_channel_disable(s_rx);
    if (err != ESP_OK) goto failed;
    /* TX stays enabled to supply shared clocks even while capturing. It
     * auto-clears DMA to silence, with PA low and DAC muted while idle. */
    s_rx_enabled = false;
    s_ready = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "16k half-duplex codec initialized; hardware audio unverified");
    return ESP_OK;
failed:
    cleanup();
    xSemaphoreGive(s_lock);
    ESP_LOGE(TAG, "Audio initialization failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t voice_board_mic_start(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_ready && !s_capture) {
        err = gpio_set_level(NM_AUDIO_AMP_ENABLE, 0);
        s_amp = false;
        if (err == ESP_OK && output_control(true) && input_control(false)) {
            err = i2s_channel_enable(s_rx); /* SDK resets RX queue on enable. */
            if (err == ESP_OK) s_rx_enabled = s_capture = true;
        } else if (err == ESP_OK) err = ESP_FAIL;
        if (err != ESP_OK) fail_closed();
    }
    xSemaphoreGive(s_lock);
    return err;
}

void voice_board_mic_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_capture = false;
    if (s_rx_enabled) {
        if (i2s_channel_disable(s_rx) != ESP_OK) fail_closed();
        s_rx_enabled = false;
    }
    if (s_ready) input_control(true);
    xSemaphoreGive(s_lock);
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak)
{
    if (peak) *peak = 0;
    if (!pcm || !frames || !s_lock) return 0;
    if (frames > NM_VOICE_CHUNK) frames = NM_VOICE_CHUNK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t bytes = 0;
    if (s_ready && s_capture) {
        const esp_err_t err = i2s_channel_read(s_rx, pcm, frames * sizeof(*pcm),
                                               &bytes, NM_CODEC_TIMEOUT_MS);
        if ((err != ESP_OK && err != ESP_ERR_TIMEOUT) ||
            bytes > frames * sizeof(*pcm) || bytes % sizeof(*pcm)) {
            fail_closed();
            bytes = 0;
        }
    }
    xSemaphoreGive(s_lock);
    const size_t count = bytes / sizeof(*pcm);
    for (size_t i = 0; peak && i < count; ++i) {
        const int magnitude = pcm[i] < 0 ? -(int)pcm[i] : (int)pcm[i];
        if (magnitude > *peak) *peak = magnitude;
    }
    return count;
}

/* Muse voice_player accepts 16k mono and expands it into three interpolated
 * 48k stereo PCM32 frames. Recover a low-cost 16k mono signal by averaging
 * all six samples, then duplicating it for the physical DAC's stereo bus.
 * int64 arithmetic handles INT32_MIN/MAX without overflow or signed shifts.
 * This box filter is for that player contract, not a general resampler.
 */
static void convert_speaker(const int32_t *input, int16_t *output, size_t frames)
{
    for (size_t i = 0; i < frames; ++i) {
        int64_t sum = 0;
        for (size_t j = 0; j < 6; ++j) sum += input[i * 6 + j];
        int64_t sample = sum / (6LL * 65536);
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
        output[2 * i] = output[2 * i + 1] = (int16_t)sample;
    }
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count)
{
    if (!frames || !count || count > SIZE_MAX / (2 * sizeof(*frames))) return ESP_ERR_INVALID_ARG;
    if (count % 3) return ESP_ERR_INVALID_SIZE;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    int16_t output[NM_VOICE_OUTPUT_CHUNK * 2];
    int16_t face_pcm[NM_VOICE_OUTPUT_CHUNK];
    while (count) {
        size_t n = count / 3;
        if (n > NM_VOICE_OUTPUT_CHUNK) n = NM_VOICE_OUTPUT_CHUNK;
        convert_speaker(frames, output, n);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (s_ready && s_amp && !s_capture) {
            size_t written = 0;
            const size_t length = n * 2 * sizeof(*output);
            err = i2s_channel_write(s_tx, output, length, &written, NM_CODEC_TIMEOUT_MS);
            if (err == ESP_OK && written != length) err = ESP_ERR_INVALID_SIZE;
            if (err != ESP_OK) fail_closed();
            else {
                for (size_t i = 0; i < n; ++i)
                    face_pcm[i] = s_volume > 0 ? output[2 * i] : 0;
                nm_face_speaker_pcm(face_pcm, n);
            }
        }
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) return err;
        frames += n * 6;
        count -= n * 3;
    }
    return ESP_OK;
}

void voice_board_amp(bool on)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = gpio_set_level(NM_AUDIO_AMP_ENABLE, 0);
    s_amp = false;
    if (err != ESP_OK) fail_closed();
    if (s_ready && (!on || !s_capture) && output_control(!on)) {
        if (on) {
            /* s_amp tracks a playback request; volume0 intentionally keeps
             * PA low while accepting/consuming silent playback frames. */
            if (!s_volume || gpio_set_level(NM_AUDIO_AMP_ENABLE, 1) == ESP_OK) s_amp = true;
            else fail_closed();
        }
    }
    xSemaphoreGive(s_lock);
}

void voice_board_set_volume(int percent)
{
    if (!s_lock) return;
    if (percent < 0) percent = 0;
    if (percent > NM_VOICE_MAX_VOLUME) percent = NM_VOICE_MAX_VOLUME;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_volume = percent;
    if (s_ready) output_control(!s_amp);
    if (gpio_set_level(NM_AUDIO_AMP_ENABLE, s_ready && s_amp && s_volume > 0) != ESP_OK)
        fail_closed();
    xSemaphoreGive(s_lock);
}

bool voice_board_muted(void)
{
    if (!s_lock) return true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* There is no separate hardware mute switch on this board. voice.c calls
     * this before mic_start: idle ADC mute must not permanently block PTT.
     * Readability/fault is the permission gate; start/stop owns ADC muting. */
    uint8_t status = 0;
    if (s_ready && (ctrl_read(&s_adc_control.base, NM_ADC_MUTE12_REG, 1, &status, 1)
                    != ESP_CODEC_DEV_OK || !controls_ok())) fail_closed();
    const bool muted = !s_ready;
    xSemaphoreGive(s_lock);
    return muted;
}

int voice_board_dial_steps(void) { return 0; }
