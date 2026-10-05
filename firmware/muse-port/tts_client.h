/* SPDX-License-Identifier: Apache-2.0
 * Nemossi's optional local Mac TTS adapter. No Muse account credentials enter it.
 */
#ifndef NEMOSSI_TTS_CLIENT_H
#define NEMOSSI_TTS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NM_TTS_TEXT_MAX 1023u
#define NM_TTS_JSON_MAX 4096u
#define NM_TTS_PCM_MAX 959956u /* 960000-byte canonical WAV, including header */
#define NM_TTS_RATE 16000u

typedef enum {
    NM_TTS_WAIT,
    NM_TTS_PCM,
    NM_TTS_DONE,
    NM_TTS_FAILED,
    NM_TTS_STALE
} nm_tts_status_t;

/* Call once during startup. Empty URL/token leaves the client disabled.
 * init never contacts a server; start queues work without waiting for I/O.
 */
bool nm_tts_init(void);
bool nm_tts_enabled(void);
uint32_t nm_tts_start(const char *text);

/* Immediately invalidates queued/in-flight/published requests. The worker
 * releases old memory and closes its socket within its next bounded wait.
 * PCM already handed to the voice task or physical DMA cannot be revoked here.
 */
void nm_tts_cancel(void);

/* One consumer (Muse session task). No waits. PCM is available only after a
 * complete, canonical PCM16 mono 16k WAV passes all checks. total_frames is
 * then exact; DONE follows the final PCM read. IDs prevent late publication.
 */
nm_tts_status_t nm_tts_read(uint32_t id, int16_t *pcm, size_t capacity,
                          size_t *frames, uint32_t *total_frames);

#ifdef __cplusplus
}
#endif
#endif
