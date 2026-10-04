#include "board_pins.h"
#include "device_services.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    int16_t samples[8] = { 123, -456 };
    size_t read_count = 99;
    const nm_pcm_format_t format = { 16000, 1, 16 };
    assert(nm_audio_capture(samples, 8, &read_count) == NM_SERVICE_UNAVAILABLE);
    assert(read_count == 0);
    assert(samples[0] == 123 && samples[1] == -456);
    assert(nm_audio_capture(NULL, 8, &read_count) == NM_SERVICE_INVALID_ARGUMENT);
    assert(read_count == 0);
    assert(nm_audio_capture(samples, 8, NULL) == NM_SERVICE_INVALID_ARGUMENT);
    assert(nm_audio_playback(samples, 8, &format) == NM_SERVICE_UNAVAILABLE);
    assert(nm_audio_playback(samples, 0, &format) == NM_SERVICE_INVALID_ARGUMENT);
    assert(nm_dot_start_call() == NM_SERVICE_UNAVAILABLE);
    assert(nm_dot_send_audio(samples, 8, &format) == NM_SERVICE_UNAVAILABLE);
    assert(nm_dot_send_audio(samples, 8, NULL) == NM_SERVICE_INVALID_ARGUMENT);
    assert(nm_dot_end_call() == NM_SERVICE_UNAVAILABLE);
    assert(strlen(nm_dot_unavailable_reason()) > 0);
    assert(strlen(nm_audio_unavailable_reason()) > 0);
    assert(NM_LCD_MOSI == 39 && NM_LCD_SCLK == 38 && NM_LCD_CS == 21);
    assert(NM_LCD_DC == 45 && NM_LCD_RESET == 40 && NM_LCD_BACKLIGHT == 46);
    assert(NM_I2C_SDA == 42 && NM_I2C_SCL == 41);
    assert(NM_TOUCH_RESET == 47 && NM_TOUCH_INTERRUPT == 48);
    assert(NM_BUTTON_PLUS == 4 && NM_BUTTON_POWER == 5 && NM_BUTTON_BOOT == 0);
    assert(NM_I2S_MCLK == 8 && NM_I2S_BCLK == 9 && NM_I2S_LRCLK == 10);
    assert(NM_I2S_DOUT == 12 && NM_I2S_DIN == 11 && NM_AUDIO_AMP_ENABLE == 7);
    puts("service boundary tests passed (no codec or dot connection claimed)");
    return 0;
}
