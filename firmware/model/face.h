/* SPDX-License-Identifier: Apache-2.0
 * Stack-chan SimpleFace adaptation; modified for Nemossi's bounded C model.
 */
#ifndef NEMOSSI_FACE_H
#define NEMOSSI_FACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "face_design.h"
#include "face_expressions.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_WIDTH STACKCHAN_TARGET_WIDTH
#define FACE_HEIGHT STACKCHAN_TARGET_HEIGHT
#define FACE_MOUTH_LEVEL_MAX 100
#define FACE_INPUT_TIMEOUT_MS 5000u
#define FACE_MOUTH_TIMEOUT_MS 750u
#define FACE_DEMO_PERIOD_MS 12000u

typedef enum {
    FACE_STATE_IDLE = 0,
    FACE_STATE_LISTENING,
    FACE_STATE_THINKING,
    FACE_STATE_SPEAKING,
    FACE_STATE_ERROR,
    FACE_STATE_BLOCKED,
    FACE_STATE_HAPPY,
    FACE_STATE_CONFUSED,
    FACE_STATE_SLEEP,
    FACE_STATE_COUNT
} face_state_t;

typedef enum {
    FACE_EVENT_IDLE = 0,
    FACE_EVENT_LISTENING,
    FACE_EVENT_THINKING,
    FACE_EVENT_SPEAKING,
    FACE_EVENT_ERROR,
    FACE_EVENT_BLOCKED,
    FACE_EVENT_MOUTH_LEVEL,
    FACE_EVENT_DEMO_ENABLE,
    FACE_EVENT_DEMO_DISABLE,
    FACE_EVENT_HAPPY,
    FACE_EVENT_CONFUSED,
    FACE_EVENT_SLEEP,
    FACE_EVENT_COUNT
} face_event_kind_t;

typedef struct {
    face_event_kind_t kind;
    int value; /* Used only by MOUTH_LEVEL; clamped to 0..100. */
} face_event_t;

typedef struct {
    face_state_t state;
    face_expression_t expression; /* Independent artwork; DEFAULT preserves state masks. */
    bool demo_mode;
    bool input_stale; /* An active input state expired; not a network status. */
    bool blink_closed;
    uint8_t mouth_level;
    uint8_t eye_open_step; /* Fixed iris, quantized upstream eyelid mask 0..12. */
    int8_t breath_offset; /* Original 320x240 vertical pixels, -6..6. */
    double gaze_x;
    double gaze_y;
    uint32_t animation_ms;
    uint32_t blink_seed;
    uint64_t state_since_ms;
    uint64_t last_input_ms;
    uint64_t last_mouth_ms;
    uint64_t last_tick_ms;
    uint64_t blink_anchor_ms;
    uint64_t demo_since_ms;
} face_model_t;

typedef struct {
    face_state_t state;
    bool demo_mode;
    bool input_stale;
    bool blink_closed;
    uint8_t mouth_level;
} face_status_t;

typedef struct {
    double eye_open;
    double gaze_x;
    double gaze_y;
    uint8_t eye_open_step;
    int8_t breath_offset;
    unsigned blink_slot;
    unsigned gaze_slot;
    unsigned blink_open_ms;
    unsigned blink_transition_ms;
    uint64_t blink_cycle_ms;
    uint64_t gaze_cycle_ms;
} face_motion_t;

/* Bounded 32-slot sampling. Seeded intervals repeat; no elapsed-time replay.
 * State changes never reset this independent monotonic motion clock. */
face_motion_t face_motion_sample(uint32_t seed, uint64_t elapsed_ms);

/* now_ms is a monotonic millisecond counter. No function sleeps or allocates. */
void face_model_init(face_model_t *model, bool demo_mode, uint64_t now_ms);
/* The seed changes bounded blink/gaze schedules; tick cadence never changes them. */
void face_model_init_seeded(face_model_t *model, bool demo_mode, uint64_t now_ms,
                            uint32_t blink_seed);

/* Selects artwork without changing lifecycle, input deadlines or motion clock.
 * Unknown expressions and invalid models are rejected without any mutation. */
bool face_model_set_expression(face_model_t *model, face_expression_t expression);

/* Rejects unknown events, backward time, mouth samples outside speaking, and
 * input events during demo. Disable demo explicitly before supplying input.
 * A rejected event leaves the model unchanged. */
bool face_model_handle_event(face_model_t *model, face_event_t event,
                             uint64_t now_ms);

/* Returns whether animation or status changed. Backward time is ignored. */
bool face_model_tick(face_model_t *model, uint64_t now_ms);
face_status_t face_model_status(const face_model_t *model);
const char *face_state_name(face_state_t state);

/* Paints a complete row-major native-endian RGB565 frame. Nominal artwork is
 * 240x240; other canvas sizes safely clip it. Rejects invalid dimensions,
 * insufficient buffers, and invalid models without writing any pixels. */
bool face_render_rgb565(const face_model_t *model, uint16_t *pixels,
                       size_t pixel_count, size_t width, size_t height);

#ifdef __cplusplus
}
#endif
#endif
