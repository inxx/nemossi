/* SPDX-License-Identifier: Apache-2.0
 * Host-only scheduling/codec fakes. No device, SDK or network calls.
 */
#ifndef NEMOSSI_MUSE_PLAYER_STUBS_H
#define NEMOSSI_MUSE_PLAYER_STUBS_H
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdatomic.h>

typedef int esp_err_t;
typedef unsigned TickType_t;
typedef unsigned EventBits_t;
typedef void *StreamBufferHandle_t;
typedef void *EventGroupHandle_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_STATE -3
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define BIT0 1u
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define MALLOC_CAP_SPIRAM 1u
#define VOICE_MIC_RATE 16000
#define VOICE_PLAYER_RATE 16000

typedef enum {
    LED_VOICE_IDLE, LED_VOICE_LISTENING, LED_VOICE_TRANSCRIBING,
    LED_VOICE_THINKING, LED_VOICE_BUFFERING, LED_VOICE_SPEAKING,
    LED_VOICE_ERROR
} led_voice_t;
typedef enum {
    MUSE_HATCH_EV_NONE, MUSE_HATCH_EV_HEARD, MUSE_HATCH_EV_REPLY,
    MUSE_HATCH_EV_DONE, MUSE_HATCH_EV_ERROR
} muse_hatch_ev_t;

void host_log(bool success, const char *format, ...);
#define ESP_LOGI(tag, ...) do { (void)(tag); host_log(true, __VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); host_log(false, __VA_ARGS__); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); host_log(false, __VA_ARGS__); } while (0)

void *heap_caps_malloc(size_t bytes, unsigned caps);
StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t bytes, size_t trigger, unsigned caps);
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t events, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t events, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t events, EventBits_t bits,
                              int clear, int all, TickType_t timeout);
int xTaskCreateWithCaps(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *handle, unsigned caps);
unsigned ulTaskNotifyTake(int clear, TickType_t timeout);
void xTaskNotifyGive(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
int vTaskSuspend(void *task);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t buffer);
size_t xStreamBufferReceive(StreamBufferHandle_t buffer, void *out, size_t bytes, TickType_t timeout);
size_t xStreamBufferSend(StreamBufferHandle_t buffer, const void *in, size_t bytes, TickType_t timeout);
int xStreamBufferIsEmpty(StreamBufferHandle_t buffer);
int xStreamBufferReset(StreamBufferHandle_t buffer);

esp_err_t voice_player_init(void);
void voice_player_begin(void);
esp_err_t voice_player_write(const int16_t *pcm, size_t frames);
void voice_player_end(void);
bool voice_player_wait(int timeout_ms);
void voice_player_stop(void);
bool voice_player_started(void);
bool voice_player_failed(void);
esp_err_t voice_board_speaker_write(const int32_t *pcm, size_t frames);
void voice_board_amp(bool on);
esp_err_t voice_board_mic_start(void);
void voice_board_mic_stop(void);
size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak);
void led_status_set_voice(led_voice_t state);
void led_status_set_level(float level);
int xQueueReceive(QueueHandle_t events, void *event, TickType_t timeout);
int64_t esp_timer_get_time(void);
void muse_hatch_turn_begin(void);
void muse_hatch_turn_cancel(void);
void muse_hatch_turn_end(void);
void muse_hatch_turn_audio(const int16_t *pcm, size_t frames);
muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t capacity);
size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms);
#endif
