#include "board_display.h"
#include "board_input.h"
#include "board_pins.h"
#include "device_services.h"
#include "face.h"

#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#error "Nemossi requires ESP-IDF 5.5.x"
#endif

_Static_assert(FACE_WIDTH == NM_LCD_WIDTH && FACE_HEIGHT == NM_LCD_HEIGHT,
               "Face framebuffer must match the board panel");

static const char *TAG = "nemossi";
static QueueHandle_t input_events;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000u;
}

static void handle_input(face_model_t *face, nm_input_event_t event, uint64_t time_ms)
{
    if (event.source != NM_INPUT_PLUS && event.source != NM_INPUT_TOUCH) return;
    ESP_LOGI(TAG, "%s %s", event.source == NM_INPUT_PLUS ? "PLUS" : "touch",
              event.pressed ? "down" : "up");
    if (!event.pressed) return;
    if (face_model_status(face).demo_mode) {
        /* Input restarts the labelled 12-second face animation. It does not
         * begin recording, playback, or an external conversation.
         */
        face_model_handle_event(face, (face_event_t){ FACE_EVENT_DEMO_ENABLE, 0 }, time_ms);
        ESP_LOGI(TAG, "DEMO restarted; audio and dot transport remain unavailable");
    } else {
        const nm_service_result_t result = nm_dot_start_call();
        if (result != NM_SERVICE_OK) {
            face_model_handle_event(face, (face_event_t){ FACE_EVENT_BLOCKED, 0 }, time_ms);
            ESP_LOGW(TAG, "%s", nm_dot_unavailable_reason());
        }
    }
}

static void ui_task(void *context)
{
    (void)context;
#ifdef CONFIG_NEMOSSI_DEMO_MODE
    const bool demo_mode = true;
#else
    const bool demo_mode = false;
#endif
    face_model_t face;
    face_model_init(&face, demo_mode, now_ms());
    if (!demo_mode) {
        face_model_handle_event(&face, (face_event_t){ FACE_EVENT_BLOCKED, 0 }, now_ms());
    }
    face_state_t logged_state = FACE_STATE_COUNT;
    TickType_t next_frame = xTaskGetTickCount();
    uint64_t last_submit_ms = now_ms();
    bool reported_stall = false;
    for (;;) {
        const uint64_t time_ms = now_ms();
        nm_input_event_t event;
        while (xQueueReceive(input_events, &event, 0) == pdTRUE) {
            handle_input(&face, event, time_ms);
        }
        face_model_tick(&face, time_ms);
        const face_status_t status = face_model_status(&face);
        if (status.state != logged_state) {
            ESP_LOGI(TAG, "face=%s mode=%s audio=unverified dot=unavailable",
                      face_state_name(status.state), status.demo_mode ? "DEMO" : "blocked");
            logged_state = status.state;
        }
        bool submitted = false;
        const esp_err_t error = nm_display_try_present(&face, &submitted);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "LCD submission failed: %s", esp_err_to_name(error));
            vTaskDelete(NULL);
            return;
        }
        if (submitted) {
            last_submit_ms = time_ms;
            reported_stall = false;
        } else if (!reported_stall && time_ms - last_submit_ms > 500u) {
            /* Keep DMA ownership until its callback; never reuse a buffer
             * merely because a timeout expired.
             */
            ESP_LOGE(TAG, "LCD DMA completion not observed; check hardware/SPI");
            reported_stall = true;
        }
        vTaskDelayUntil(&next_frame, pdMS_TO_TICKS(1000 / CONFIG_NEMOSSI_UI_FPS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Waveshare ESP32-S3-Touch-LCD-1.54 face bring-up (ESP-IDF %s)", IDF_VER);
    ESP_LOGW(TAG, "%s", nm_audio_unavailable_reason());
    ESP_LOGW(TAG, "%s", nm_dot_unavailable_reason());
    input_events = xQueueCreate(8, sizeof(nm_input_event_t));
    if (!input_events) {
        ESP_LOGE(TAG, "Could not allocate input queue");
        return;
    }
    esp_err_t error = nm_display_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "LCD initialization failed: %s", esp_err_to_name(error));
        vQueueDelete(input_events);
        input_events = NULL;
        return;
    }
    error = nm_input_init(input_events);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Input initialization failed: %s; face demo still available",
                  esp_err_to_name(error));
    }
    if (xTaskCreate(ui_task, "nm_face", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start face UI task");
    }
}
