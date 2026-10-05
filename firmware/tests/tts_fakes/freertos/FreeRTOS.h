#ifndef NM_TTS_FAKE_FREERTOS_H
#define NM_TTS_FAKE_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#endif
