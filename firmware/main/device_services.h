#ifndef NEMOSSI_DEVICE_SERVICES_H
#define NEMOSSI_DEVICE_SERVICES_H

#include <stddef.h>
#include <stdint.h>

/* Separate device I/O and external dot transport contracts. Replacing these
 * adapters requires a verified protocol and physical codec bring-up first.
 */
typedef enum {
    NM_SERVICE_OK = 0,
    NM_SERVICE_INVALID_ARGUMENT,
    NM_SERVICE_UNAVAILABLE
} nm_service_result_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
} nm_pcm_format_t;

const char *nm_audio_unavailable_reason(void);
const char *nm_dot_unavailable_reason(void);
nm_service_result_t nm_audio_capture(int16_t *samples, size_t capacity,
                                     size_t *samples_read);
nm_service_result_t nm_audio_playback(const int16_t *samples, size_t count,
                                      const nm_pcm_format_t *format);
nm_service_result_t nm_dot_start_call(void);
nm_service_result_t nm_dot_send_audio(const int16_t *samples, size_t count,
                                      const nm_pcm_format_t *format);
nm_service_result_t nm_dot_end_call(void);

#endif
