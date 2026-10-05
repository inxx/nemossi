#pragma once
#include <stddef.h>
#include <stdint.h>

/* Called after a successful bounded speaker write. No audio is saved. */
void nm_face_speaker_pcm(const int16_t *pcm, size_t samples);
