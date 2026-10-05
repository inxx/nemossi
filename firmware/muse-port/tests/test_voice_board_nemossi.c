/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define NEMOSSI_VOICE_HOST_TEST 1
#include "../main/voice_board_nemossi.c"

struct fake_i2c { uint8_t registers[256]; bool attached; unsigned address; };
struct fake_i2s { bool enabled; };
struct fake_codec_device { const audio_codec_if_t *codec; bool output; };
static struct fake_i2c devices[2];
static struct fake_i2s tx_hw, rx_hw;
static struct fake_codec_device output_hw, input_hw;
static audio_codec_if_t dac_hw, adc_hw;
static i2s_std_config_t tx_config;
static i2s_tdm_config_t rx_config;
static esp_codec_dev_sample_info_t tx_format, rx_format;
static int amp_level, requested_volume, lock_depth;
static bool bus_fail, write_fail, read_fail, readback_wrong, format_fail, enable_fail;
static bool constructor_write_fail;
static bool short_write;
static esp_err_t read_error;
static size_t read_length;
static int16_t read_pcm[NM_VOICE_CHUNK];
static int16_t output_pcm[NM_VOICE_CHUNK * 2];
static size_t output_samples;
static int16_t face_samples[NM_VOICE_CHUNK];
static size_t face_count;

void nm_face_speaker_pcm(const int16_t *pcm, size_t samples) {
    assert(face_count + samples <= sizeof(face_samples) / sizeof(*face_samples));
    memcpy(face_samples + face_count, pcm, samples * sizeof(*pcm));
    face_count += samples;
}

const char *esp_err_to_name(esp_err_t error) { (void)error; return "host fake"; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &lock_depth; }
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout) {
    (void)lock; (void)timeout; assert(lock_depth == 0); lock_depth++; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t lock) {
    (void)lock; assert(lock_depth == 1); lock_depth--; return 1;
}
esp_err_t gpio_config(const gpio_config_t *config) {
    assert(config->pin_bit_mask == (1ULL << NM_AUDIO_AMP_ENABLE));
    assert(config->mode == GPIO_MODE_OUTPUT); return ESP_OK;
}
esp_err_t gpio_set_level(int pin, unsigned level) {
    assert(pin == NM_AUDIO_AMP_ENABLE && level <= 1); amp_level = (int)level; return ESP_OK;
}
esp_err_t nm_i2c_bus_get(i2c_master_bus_handle_t *bus) {
    if (bus_fail) return ESP_FAIL;
    *bus = devices; return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
    const i2c_device_config_t *config, i2c_master_dev_handle_t *device) {
    assert(bus == devices && config->scl_speed_hz == NM_I2C_HZ);
    assert(config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    assert(config->device_address == 0x18 || config->device_address == 0x40);
    *device = &devices[config->device_address == 0x18 ? 0 : 1];
    assert(!(*device)->attached);
    (*device)->address = config->device_address;
    (*device)->attached = true; return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) {
    assert(device->attached); device->attached = false; return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
    const uint8_t *out, size_t out_size, uint8_t *in, size_t in_size, int timeout) {
    assert(device->attached && out_size == 1 && in_size == 1 && timeout == 100);
    if (read_fail) return ESP_FAIL;
    *in = device->registers[*out];
    if (readback_wrong && device->address == 0x18 && *out == 0x32) *in ^= 1;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
    const uint8_t *out, size_t size, int timeout) {
    assert(device->attached && size == 2 && timeout == 100);
    if (write_fail) return ESP_FAIL;
    if (constructor_write_fail && out[0] == 1) {
        constructor_write_fail = false; return ESP_FAIL;
    }
    device->registers[out[0]] = out[1]; return ESP_OK;
}
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx) {
    assert(config->id == 0 && config->role == I2S_ROLE_MASTER);
    assert(config->dma_frame_num == 320 && config->dma_desc_num == 6);
    assert(config->auto_clear_after_cb);
    *tx = &tx_hw; *rx = &rx_hw; return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *config) {
    assert(channel == &tx_hw); tx_config = *config; return ESP_OK;
}
esp_err_t i2s_channel_init_tdm_mode(i2s_chan_handle_t channel, const i2s_tdm_config_t *config) {
    assert(channel == &rx_hw); rx_config = *config; return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t channel) {
    if (enable_fail) return ESP_FAIL;
    if (channel->enabled) return ESP_ERR_INVALID_STATE;
    channel->enabled = true; return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t channel) {
    if (!channel->enabled) return ESP_ERR_INVALID_STATE;
    channel->enabled = false; return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t channel) { channel->enabled = false; return ESP_OK; }
esp_err_t i2s_channel_read(i2s_chan_handle_t channel, void *data, size_t size, size_t *read, uint32_t timeout) {
    (void)timeout; assert(channel == &rx_hw); *read = 0;
    if (!channel->enabled) return ESP_ERR_INVALID_STATE;
    if (!size) return ESP_OK;
    *read = read_length < size ? read_length : size;
    memcpy(data, read_pcm, *read); return read_error;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t channel, const void *data, size_t size, size_t *written, uint32_t timeout) {
    (void)timeout; assert(channel == &tx_hw); *written = 0;
    if (!channel->enabled) return ESP_ERR_INVALID_STATE;
    if (short_write && size) { *written = size - 2; return ESP_OK; }
    assert(output_samples + size / 2 <= sizeof(output_pcm) / sizeof(*output_pcm));
    memcpy(output_pcm + output_samples, data, size);
    output_samples += size / 2;
    *written = size; return ESP_OK;
}
static int data_format(const audio_codec_data_if_t *data, esp_codec_dev_type_t type,
                        esp_codec_dev_sample_info_t *format) {
    (void)data;
    if (format_fail) return ESP_CODEC_DEV_DRV_ERR;
    if (type == ESP_CODEC_DEV_TYPE_OUT) tx_format = *format;
    else rx_format = *format;
    return ESP_CODEC_DEV_OK;
}
static const audio_codec_data_if_t fake_data = {.set_fmt = data_format};
const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *config) {
    assert(config->port == 0 && config->tx_handle == &tx_hw && config->rx_handle == &rx_hw);
    return &fake_data;
}
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *config) {
    assert(config->pa_pin == -1 && config->use_mclk && config->mclk_div == 256);
    dac_hw = (audio_codec_if_t){.control = config->ctrl_if, .output = true};
    uint8_t value = 1;
    /* Model a vendor constructor overwriting the first write's return code. */
    config->ctrl_if->write_reg(config->ctrl_if, 1, 1, &value, 1);
    config->ctrl_if->write_reg(config->ctrl_if, 2, 1, &value, 1);
    return &dac_hw;
}
const audio_codec_if_t *es7210_codec_new(es7210_codec_cfg_t *config) {
    assert(config->mic_selected == 15 && config->mclk_div == 256);
    adc_hw = (audio_codec_if_t){.control = config->ctrl_if}; return &adc_hw;
}
esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *config) {
    struct fake_codec_device *device = config->dev_type == ESP_CODEC_DEV_TYPE_OUT ? &output_hw : &input_hw;
    device->codec = config->codec_if;
    device->output = config->dev_type == ESP_CODEC_DEV_TYPE_OUT; return device;
}
int esp_codec_dev_open(esp_codec_dev_handle_t device, esp_codec_dev_sample_info_t *format) {
    (void)format;
    /* Vendor open ignores the result of data_if->enable. */
    i2s_channel_enable(device->output ? &tx_hw : &rx_hw);
    return ESP_CODEC_DEV_OK;
}
int esp_codec_dev_close(esp_codec_dev_handle_t device) { (void)device; return ESP_CODEC_DEV_OK; }
void esp_codec_dev_delete(esp_codec_dev_handle_t device) { (void)device; }
int audio_codec_delete_codec_if(const audio_codec_if_t *codec) { (void)codec; return ESP_CODEC_DEV_OK; }
int audio_codec_delete_data_if(const audio_codec_data_if_t *data) { (void)data; return ESP_CODEC_DEV_OK; }
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t device, int percent) {
    requested_volume = percent;
    const uint8_t value = (uint8_t)(percent + 42);
    device->codec->control->write_reg(device->codec->control, 0x32, 1, (void *)&value, 1);
    return ESP_CODEC_DEV_OK; /* Real wrapper ignores the codec write result. */
}
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t device, bool mute) {
    const uint8_t value = mute ? 0x60 : 0;
    device->codec->control->write_reg(device->codec->control, 0x31, 1, (void *)&value, 1);
    return ESP_CODEC_DEV_OK;
}
int esp_codec_dev_set_in_mute(esp_codec_dev_handle_t device, bool mute) {
    const uint8_t value = mute ? 3 : 0;
    device->codec->control->write_reg(device->codec->control, 0x14, 1, (void *)&value, 1);
    device->codec->control->write_reg(device->codec->control, 0x15, 1, (void *)&value, 1);
    return ESP_CODEC_DEV_OK;
}
int esp_codec_dev_set_in_channel_gain(esp_codec_dev_handle_t device, uint16_t channels, float gain) {
    assert(device == &input_hw && channels == 1 && gain == 30.0f); return ESP_CODEC_DEV_OK;
}

static void reset(void) {
    cleanup();
    memset(devices, 0, sizeof(devices));
    memset(&s_dac_control, 0, sizeof(s_dac_control));
    memset(&s_adc_control, 0, sizeof(s_adc_control));
    bus_fail = write_fail = read_fail = readback_wrong = format_fail = enable_fail = false;
    constructor_write_fail = short_write = false;
    read_error = ESP_OK; read_length = output_samples = face_count = 0;
    assert(lock_depth == 0 && amp_level == 0);
}

static void test_init_contract(void) {
    reset(); assert(voice_board_init() == ESP_OK);
    assert(s_ready && !voice_board_muted() && amp_level == 0);
    assert(requested_volume == 20 && devices[0].registers[0x31] == 0x60);
    assert(devices[1].registers[0x14] == 3 && !rx_hw.enabled && tx_hw.enabled);
    assert(tx_config.clk_cfg.sample_rate_hz == 16000 && rx_config.clk_cfg.sample_rate_hz == 16000);
    assert(tx_config.slot_cfg.slot_bit_width == 32 && tx_config.slot_cfg.ws_width == 32);
    assert(rx_config.slot_cfg.slot_bit_width == 16 && rx_config.slot_cfg.total_slot == 4);
    assert(rx_config.slot_cfg.slot_mask == 1 && rx_config.slot_cfg.ws_width == 32 && rx_config.slot_cfg.left_align);
    assert(tx_format.channel == 2 && rx_format.channel == 4 && rx_format.channel_mask == 1);
    assert(tx_format.mclk_multiple == 256 && rx_format.mclk_multiple == 256);
    assert(tx_config.gpio_cfg.dout == 12 && rx_config.gpio_cfg.din == 11);
    assert(tx_config.gpio_cfg.mclk == 8 && tx_config.gpio_cfg.bclk == 9 && tx_config.gpio_cfg.ws == 10);
    assert(voice_board_dial_steps() == 0);
    assert(voice_board_init() == ESP_OK); /* No second I2C attachment. */
}
static void test_fail_closed_controls(void) {
    reset(); bus_fail = true; assert(voice_board_init() != ESP_OK && amp_level == 0);
    reset(); constructor_write_fail = true;
    assert(voice_board_init() != ESP_OK && !s_ready && !devices[0].attached && amp_level == 0);
    reset(); format_fail = true; assert(voice_board_init() != ESP_OK && !s_ready);
    reset(); enable_fail = true; assert(voice_board_init() != ESP_OK && !s_ready);
    reset(); assert(voice_board_init() == ESP_OK);
    voice_board_amp(true); assert(amp_level == 1);
    write_fail = true; voice_board_set_volume(10);
    assert(!s_ready && amp_level == 0 && voice_board_muted());
    reset(); assert(voice_board_init() == ESP_OK);
    readback_wrong = true; voice_board_amp(true);
    assert(!s_ready && amp_level == 0);
    reset(); assert(voice_board_init() == ESP_OK);
    read_fail = true; assert(voice_board_muted() && !s_ready && amp_level == 0);
}
static void test_volume_and_half_duplex(void) {
    reset(); assert(voice_board_init() == ESP_OK);
    voice_board_set_volume(100); assert(requested_volume == 20);
    voice_board_amp(true); assert(amp_level == 1);
    voice_board_set_volume(-1); assert(requested_volume == 0 && amp_level == 0);
    const int32_t silence[6] = {0};
    assert(voice_board_speaker_write(silence, 3) == ESP_OK);
    assert(face_count == 1 && face_samples[0] == 0);
    voice_board_set_volume(10); assert(requested_volume == 10 && amp_level == 1);
    assert(voice_board_mic_start() == ESP_OK && amp_level == 0 && rx_hw.enabled && tx_hw.enabled);
    assert(devices[1].registers[0x14] == 0 && devices[0].registers[0x31] == 0x60);
    voice_board_amp(true); assert(amp_level == 0);
    assert(voice_board_speaker_write(silence, 3) == ESP_ERR_INVALID_STATE);
    voice_board_mic_stop(); assert(!rx_hw.enabled && tx_hw.enabled);
    assert(devices[1].registers[0x14] == 3 && !voice_board_muted());
    assert(voice_board_mic_start() == ESP_OK); voice_board_mic_stop();
}
static void test_capture_boundaries(void) {
    reset(); assert(voice_board_init() == ESP_OK);
    int16_t pcm[320]; int peak = -1;
    assert(voice_board_mic_read(pcm, 320, &peak) == 0 && peak == 0);
    assert(voice_board_mic_start() == ESP_OK);
    read_pcm[0] = INT16_MIN; read_pcm[1] = INT16_MAX; read_pcm[2] = -3;
    read_length = 6; read_error = ESP_ERR_TIMEOUT;
    assert(voice_board_mic_read(pcm, 320, &peak) == 3 && peak == 32768 && pcm[2] == -3 && s_ready);
    read_length = 3;
    assert(voice_board_mic_read(pcm, 320, &peak) == 0 && !s_ready && amp_level == 0);
    voice_board_mic_stop(); assert(!rx_hw.enabled);
}
static void test_speaker_conversion_and_errors(void) {
    reset(); assert(voice_board_init() == ESP_OK); voice_board_amp(true);
    int32_t input[6 * 65];
    for (size_t i = 0; i < 65; ++i) for (size_t j = 0; j < 6; ++j) input[6*i+j] = (int32_t)(i * 65536);
    assert(voice_board_speaker_write(input, 65 * 3) == ESP_OK && output_samples == 130);
    assert(face_count == 65);
    for (size_t i = 0; i < 65; ++i) {
        assert(output_pcm[2*i] == (int)i && output_pcm[2*i+1] == (int)i);
        assert(face_samples[i] == (int)i);
    }
    int16_t converted[2];
    for (size_t j = 0; j < 6; ++j) input[j] = INT32_MAX;
    convert_speaker(input, converted, 1); assert(converted[0] == INT16_MAX && converted[1] == INT16_MAX);
    for (size_t j = 0; j < 6; ++j) input[j] = INT32_MIN;
    convert_speaker(input, converted, 1); assert(converted[0] == INT16_MIN);
    for (size_t j = 0; j < 6; ++j) input[j] = j % 2 ? INT32_MIN : INT32_MAX;
    convert_speaker(input, converted, 1); assert(converted[0] == 0);
    assert(voice_board_speaker_write(input, 2) == ESP_ERR_INVALID_SIZE);
    assert(voice_board_speaker_write(NULL, 3) == ESP_ERR_INVALID_ARG);
    assert(voice_board_speaker_write(input, SIZE_MAX) == ESP_ERR_INVALID_ARG);
    short_write = true;
    assert(voice_board_speaker_write(input, 3) == ESP_ERR_INVALID_SIZE && !s_ready && amp_level == 0);
    assert(face_count == 65); /* Failed writes never animate speaking. */
}
int main(void) {
    test_init_contract();
    test_fail_closed_controls();
    test_volume_and_half_duplex();
    test_capture_boundaries();
    test_speaker_conversion_and_errors();
    reset();
    puts("Nemossi Muse audio: 5 host test groups passed (hardware unverified)");
    return 0;
}
