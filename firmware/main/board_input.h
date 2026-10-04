#ifndef NEMOSSI_BOARD_INPUT_H
#define NEMOSSI_BOARD_INPUT_H

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum { NM_INPUT_PLUS, NM_INPUT_TOUCH } nm_input_source_t;
typedef struct {
    nm_input_source_t source;
    bool pressed;
} nm_input_event_t;

/* PLUS is active-low GPIO4. Touch is optional. POWER and BOOT are untouched. */
esp_err_t nm_input_init(QueueHandle_t events);

#endif
