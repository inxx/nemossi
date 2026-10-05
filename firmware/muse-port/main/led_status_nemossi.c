/* SPDX-License-Identifier: Apache-2.0
 * Muse status adapter for the existing Nemossi Stack-chan face.
 * GPIO provenance: Waveshare 5157db7c888e476fd57f8a95020800377447478f.
 */
#include "led_status.h"
#include "nemossi_face.h"
#include "board_display.h"
#include "board_input.h"
#include "face.h"
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static atomic_int connection_state = LED_STATE_BOOT;
static atomic_int voice_state = LED_VOICE_IDLE;
static atomic_uint speaker_level;
static atomic_uint speaker_at_ms;
static QueueHandle_t touch_events;
static bool display_ready;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void nm_face_speaker_pcm(const int16_t *pcm, size_t samples)
{
    if (!pcm || !samples) return;
    unsigned peak = 0;
    for (size_t i = 0; i < samples; ++i) {
        int value = pcm[i];
        unsigned magnitude = (unsigned)(value < 0 ? -value : value);
        if (magnitude > peak) peak = magnitude;
    }
    unsigned level = peak * 100u / 12000u;
    atomic_store(&speaker_level, level > 100u ? 100u : level);
    atomic_store(&speaker_at_ms, now_ms());
}

static face_event_kind_t status_event(void)
{
    switch (atomic_load(&voice_state)) {
    case LED_VOICE_LISTENING: return FACE_EVENT_LISTENING;
    case LED_VOICE_TRANSCRIBING:
    case LED_VOICE_THINKING:
    case LED_VOICE_BUFFERING: return FACE_EVENT_THINKING;
    case LED_VOICE_SPEAKING: return FACE_EVENT_SPEAKING;
    case LED_VOICE_ERROR: return FACE_EVENT_ERROR;
    default: break;
    }
    switch (atomic_load(&connection_state)) {
    case LED_STATE_ERROR: return FACE_EVENT_ERROR;
    case LED_STATE_AUTH_OK:
    case LED_STATE_VM_OK:
    case LED_STATE_WS_CONNECTED: return FACE_EVENT_IDLE;
    default: return FACE_EVENT_BLOCKED;
    }
}

static void face_task(void *unused)
{
    (void)unused;
    face_model_t face;
    face_model_init(&face, false, (uint64_t)esp_timer_get_time() / 1000u);
    face_expression_t expression = FACE_EXPRESSION_DEFAULT;
    TickType_t next = xTaskGetTickCount();
    bool warned = false;
    for (;;) {
        uint64_t time_ms = (uint64_t)esp_timer_get_time() / 1000u;
        nm_input_event_t input;
        while (touch_events && xQueueReceive(touch_events, &input, 0) == pdTRUE) {
            /* PLUS belongs to Muse's PTT/setup button. Touch selects artwork. */
            if (input.source == NM_INPUT_TOUCH && input.pressed) {
                expression = (face_expression_t)((expression + 1) % FACE_EXPRESSION_COUNT);
                face_model_set_expression(&face, expression);
            }
        }
        face_event_kind_t event = status_event();
        face_model_handle_event(&face, (face_event_t){event, 0}, time_ms);
        if (event == FACE_EVENT_SPEAKING) {
            unsigned age = (uint32_t)time_ms - atomic_load(&speaker_at_ms);
            int level = age <= 150u ? (int)atomic_load(&speaker_level) : 0;
            face_model_handle_event(&face, (face_event_t){FACE_EVENT_MOUTH_LEVEL, level}, time_ms);
        }
        face_model_tick(&face, time_ms);
        if (display_ready) {
            bool submitted = false;
            esp_err_t error = nm_display_try_present(&face, &submitted);
            if (error != ESP_OK && !warned) {
                ESP_LOGE("link.nemossi_face", "display failed: %s", esp_err_to_name(error));
                warned = true;
            }
        }
        vTaskDelayUntil(&next, pdMS_TO_TICKS(33));
    }
}

bool led_status_init(void)
{
    display_ready = nm_display_init() == ESP_OK;
    touch_events = xQueueCreate(8, sizeof(nm_input_event_t));
    if (touch_events && nm_input_init(touch_events) != ESP_OK)
        ESP_LOGW("link.nemossi_face", "touch unavailable; default face retained");
    if (xTaskCreate(face_task, "nm_face", 4096, NULL, 4, NULL) != pdPASS) return false;
    return display_ready;
}
void led_status_set_state(led_state_t state) { atomic_store(&connection_state, state); }
void led_status_set_voice(led_voice_t voice) { atomic_store(&voice_state, voice); }
void led_status_set_level(float level) { (void)level; }
void led_status_show_volume(int percent) { (void)percent; }
void led_status_set_title(const char *title) { (void)title; }
bool led_status_display_info(int *width, int *height)
{ if (width) *width = FACE_WIDTH; if (height) *height = FACE_HEIGHT; return display_ready; }
int led_status_display_bits(void) { return display_ready ? 16 : 0; }
bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels)
{ (void)x; (void)y; (void)w; (void)h; (void)pixels; return false; }
void led_status_draw_done(void) {}
void led_status_show_animation(void) {}
