#include "device_services.h"

static int valid_pcm(const nm_pcm_format_t *format)
{
    return format && format->sample_rate_hz > 0 && format->channels > 0 &&
           format->bits_per_sample == 16;
}

const char *nm_audio_unavailable_reason(void)
{
    return "ES7210 capture / ES8311 playback have not been brought up on hardware";
}

const char *nm_dot_unavailable_reason(void)
{
    return "No officially supported external-device transport for the existing dot call is verified";
}

nm_service_result_t nm_audio_capture(int16_t *samples, size_t capacity,
                                     size_t *samples_read)
{
    if (samples_read) *samples_read = 0;
    if (!samples || !capacity || !samples_read) return NM_SERVICE_INVALID_ARGUMENT;
    return NM_SERVICE_UNAVAILABLE;
}

nm_service_result_t nm_audio_playback(const int16_t *samples, size_t count,
                                      const nm_pcm_format_t *format)
{
    if (!samples || !count || !valid_pcm(format)) return NM_SERVICE_INVALID_ARGUMENT;
    return NM_SERVICE_UNAVAILABLE;
}

nm_service_result_t nm_dot_start_call(void)
{
    return NM_SERVICE_UNAVAILABLE;
}

nm_service_result_t nm_dot_send_audio(const int16_t *samples, size_t count,
                                      const nm_pcm_format_t *format)
{
    if (!samples || !count || !valid_pcm(format)) return NM_SERVICE_INVALID_ARGUMENT;
    return NM_SERVICE_UNAVAILABLE;
}

nm_service_result_t nm_dot_end_call(void)
{
    return NM_SERVICE_UNAVAILABLE;
}
