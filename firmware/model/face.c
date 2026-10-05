/* SPDX-License-Identifier: Apache-2.0
 * Geometry, eyelid masks, mouth and easing adapted from Stack-chan SimpleFace
 * by meganetaaan, commit 2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d.
 * Modified for Nemossi: RGB565 projection, bounded seeded motion, safety model.
 */
#include "face.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
    COLOR_BACKGROUND = 0x0000,
    COLOR_FACE = 0xFFFF
};

static bool model_valid(const face_model_t *model)
{
    return model != NULL && (unsigned)model->state < FACE_STATE_COUNT &&
           model->mouth_level <= FACE_MOUTH_LEVEL_MAX &&
           model->eye_open_step <= STACKCHAN_EYE_OPEN_STEPS &&
           model->breath_offset >= -(int)STACKCHAN_BREATH_AMPLITUDE &&
           model->breath_offset <= (int)STACKCHAN_BREATH_AMPLITUDE &&
           isfinite(model->gaze_x) && fabs(model->gaze_x) <= 2.0 &&
           isfinite(model->gaze_y) && fabs(model->gaze_y) <= 2.0 &&
           model->state_since_ms <= model->last_tick_ms &&
           model->last_input_ms <= model->last_tick_ms &&
           model->last_mouth_ms <= model->last_tick_ms &&
           model->blink_anchor_ms <= model->last_tick_ms &&
           model->demo_since_ms <= model->last_tick_ms;
}

static bool state_active(face_state_t state)
{
    return state == FACE_STATE_LISTENING || state == FACE_STATE_THINKING ||
           state == FACE_STATE_SPEAKING;
}

static void set_state(face_model_t *model, face_state_t state, uint64_t now_ms)
{
    if (model->state != state) {
        model->state = state;
        model->state_since_ms = now_ms;
        model->mouth_level = 0;
    }
}

/* A deterministic integer triangle wave, including both endpoints. */
static unsigned triangle(uint64_t elapsed_ms, unsigned period_ms,
                         unsigned maximum)
{
    const unsigned half = period_ms / 2u;
    unsigned phase = (unsigned)(elapsed_ms % period_ms);
    if (phase > half) {
        phase = period_ms - phase;
    }
    return phase * maximum / half;
}

void face_model_init(face_model_t *model, bool demo_mode, uint64_t now_ms)
{
    face_model_init_seeded(model, demo_mode, now_ms, STACKCHAN_ANIMATION_SEED);
}

void face_model_init_seeded(face_model_t *model, bool demo_mode, uint64_t now_ms,
                            uint32_t blink_seed)
{
    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
    model->state = FACE_STATE_IDLE;
    model->demo_mode = demo_mode;
    model->blink_seed = blink_seed;
    model->state_since_ms = now_ms;
    model->last_input_ms = now_ms;
    model->last_mouth_ms = now_ms;
    model->last_tick_ms = now_ms;
    model->blink_anchor_ms = now_ms;
    model->demo_since_ms = now_ms;
    model->eye_open_step = STACKCHAN_EYE_OPEN_STEPS;
}

static uint32_t hash32(uint32_t value)
{
    value ^= value >> 16;
    value *= UINT32_C(0x7FEB352D);
    value ^= value >> 15;
    value *= UINT32_C(0x846CA68B);
    return value ^ (value >> 16);
}

static double uniform(uint32_t seed, unsigned slot, unsigned lane)
{
    return hash32(seed ^ ((slot + 1u) * UINT32_C(0x9E3779B9)) ^
                  ((lane + 1u) * UINT32_C(0x85EBCA6B))) / 4294967296.0;
}

static unsigned duration(uint32_t seed, unsigned slot, unsigned lane,
                         unsigned minimum, unsigned maximum)
{
    return minimum + (unsigned)floor(uniform(seed, slot, lane) * (maximum - minimum));
}

static double normal(uint32_t seed, unsigned slot, unsigned lane)
{
    const double a = 1.0 - uniform(seed, slot, lane);
    const double b = 1.0 - uniform(seed, slot, lane + 1u);
    const double angle = 6.28318530717958647692 * b;
    const double unit = uniform(seed, slot, lane + 2u) < 0.5 ? sin(angle) : cos(angle);
    return sqrt(-2.0 * log(a)) * unit * STACKCHAN_GAZE_GAIN_MILLI / 1000.0;
}

face_motion_t face_motion_sample(uint32_t seed, uint64_t elapsed_ms)
{
    face_motion_t sample = {0};
    unsigned opens[STACKCHAN_ANIMATION_SLOTS];
    unsigned closes[STACKCHAN_ANIMATION_SLOTS];
    unsigned gazes[STACKCHAN_ANIMATION_SLOTS];
    const uint64_t time = elapsed_ms - elapsed_ms % STACKCHAN_MOTION_TICK_MS;
    for (unsigned slot = 0; slot < STACKCHAN_ANIMATION_SLOTS; ++slot) {
        opens[slot] = duration(seed, slot, 0, STACKCHAN_BLINK_OPEN_MIN_MS, STACKCHAN_BLINK_OPEN_MAX_MS);
        closes[slot] = duration(seed, slot, 1, STACKCHAN_BLINK_TRANSITION_MIN_MS, STACKCHAN_BLINK_TRANSITION_MAX_MS);
        gazes[slot] = duration(seed, slot, 2, STACKCHAN_GAZE_INTERVAL_MIN_MS, STACKCHAN_GAZE_INTERVAL_MAX_MS);
        sample.blink_cycle_ms += opens[slot] + closes[slot];
        sample.gaze_cycle_ms += gazes[slot];
    }
    uint64_t phase = time % sample.blink_cycle_ms;
    while (sample.blink_slot + 1u < STACKCHAN_ANIMATION_SLOTS &&
           phase >= opens[sample.blink_slot] + closes[sample.blink_slot]) {
        phase -= opens[sample.blink_slot] + closes[sample.blink_slot];
        ++sample.blink_slot;
    }
    sample.blink_open_ms = opens[sample.blink_slot];
    sample.blink_transition_ms = closes[sample.blink_slot];
    sample.eye_open = 1.0;
    if (phase >= sample.blink_open_ms) {
        const double fraction = (double)(phase - sample.blink_open_ms) / sample.blink_transition_ms;
        const double ease = fraction < 0.25 ? 1.0 - fraction * 4.0 :
            (fraction - 0.25) * (fraction - 0.25) * 16.0 / 9.0;
        const double minimum = STACKCHAN_BLINK_MIN_OPEN_MILLI / 1000.0;
        sample.eye_open = minimum + ease * (1.0 - minimum);
    }
    sample.eye_open_step = (uint8_t)floor(sample.eye_open * STACKCHAN_EYE_OPEN_STEPS + 0.5);
    phase = time % sample.gaze_cycle_ms;
    while (sample.gaze_slot + 1u < STACKCHAN_ANIMATION_SLOTS && phase >= gazes[sample.gaze_slot]) {
        phase -= gazes[sample.gaze_slot];
        ++sample.gaze_slot;
    }
    if (sample.gaze_slot != 0) {
        sample.gaze_x = normal(seed, sample.gaze_slot, 3);
        sample.gaze_y = normal(seed, sample.gaze_slot, 6);
    }
    const double angle = 6.28318530717958647692 * (double)(time % STACKCHAN_BREATH_PERIOD_MS) /
        STACKCHAN_BREATH_PERIOD_MS;
    const double breath = ceil(sin(angle) * STACKCHAN_BREATH_STEPS) / STACKCHAN_BREATH_STEPS;
    sample.breath_offset = (int8_t)floor(breath * STACKCHAN_BREATH_AMPLITUDE + 0.5);
    return sample;
}

static void animate(face_model_t *model, uint64_t now_ms)
{
    const uint64_t elapsed = now_ms - model->blink_anchor_ms;
    model->animation_ms = (uint32_t)(elapsed % 60000u);
    const face_motion_t motion = face_motion_sample(model->blink_seed, elapsed);
    model->eye_open_step = model->state == FACE_STATE_SLEEP ? 0 : motion.eye_open_step;
    model->blink_closed = model->eye_open_step <= 2;
    model->breath_offset = motion.breath_offset;
    model->gaze_x = motion.gaze_x;
    model->gaze_y = motion.gaze_y;
}

bool face_model_tick(face_model_t *model, uint64_t now_ms)
{
    if (!model_valid(model) || now_ms < model->last_tick_ms) {
        return false;
    }
    const face_status_t previous = face_model_status(model);
    const uint32_t previous_animation = model->animation_ms;
    const uint8_t previous_eye = model->eye_open_step;
    const int8_t previous_breath = model->breath_offset;
    const double previous_gaze_x = model->gaze_x;
    const double previous_gaze_y = model->gaze_y;
    model->last_tick_ms = now_ms;

    if (model->demo_mode) {
        const uint64_t elapsed = now_ms - model->demo_since_ms;
        const unsigned phase = (unsigned)(elapsed % FACE_DEMO_PERIOD_MS);
        const unsigned slot = phase / 3000u;
        const face_state_t demo_states[] = {
            FACE_STATE_IDLE, FACE_STATE_LISTENING,
            FACE_STATE_THINKING, FACE_STATE_SPEAKING
        };
        model->state = demo_states[slot];
        /* Recover the actual phase boundary, even after a large skipped tick. */
        model->state_since_ms = now_ms - phase + slot * 3000u;
        model->input_stale = false;
        model->mouth_level = model->state == FACE_STATE_SPEAKING
            ? (uint8_t)triangle(elapsed, 500u, FACE_MOUTH_LEVEL_MAX) : 0;
    } else {
        if (state_active(model->state) &&
            now_ms - model->last_input_ms >= FACE_INPUT_TIMEOUT_MS) {
            set_state(model, FACE_STATE_IDLE,
                      model->last_input_ms + FACE_INPUT_TIMEOUT_MS);
            model->input_stale = true;
        }
        if (model->state == FACE_STATE_SPEAKING &&
            now_ms - model->last_mouth_ms >= FACE_MOUTH_TIMEOUT_MS) {
            model->mouth_level = 0;
        }
    }
    animate(model, now_ms);
    const face_status_t current = face_model_status(model);
    return previous.state != current.state ||
           previous.demo_mode != current.demo_mode ||
           previous.input_stale != current.input_stale ||
           previous.blink_closed != current.blink_closed ||
           previous.mouth_level != current.mouth_level ||
           previous_animation != model->animation_ms ||
           previous_eye != model->eye_open_step || previous_breath != model->breath_offset ||
           previous_gaze_x != model->gaze_x || previous_gaze_y != model->gaze_y;
}

bool face_model_handle_event(face_model_t *model, face_event_t event,
                             uint64_t now_ms)
{
    if (!model_valid(model) || (unsigned)event.kind >= FACE_EVENT_COUNT ||
        now_ms < model->last_tick_ms) {
        return false;
    }
    const bool demo_event = event.kind == FACE_EVENT_DEMO_ENABLE ||
                            event.kind == FACE_EVENT_DEMO_DISABLE;
    if ((model->demo_mode && !demo_event) ||
        (event.kind == FACE_EVENT_MOUTH_LEVEL &&
         (model->state != FACE_STATE_SPEAKING ||
          now_ms - model->last_input_ms >= FACE_INPUT_TIMEOUT_MS))) {
        return false;
    }
    (void)face_model_tick(model, now_ms);
    if (demo_event) {
        model->demo_mode = event.kind == FACE_EVENT_DEMO_ENABLE;
        model->demo_since_ms = now_ms;
        model->state = FACE_STATE_IDLE;
        model->state_since_ms = now_ms;
        model->animation_ms = 0;
        model->mouth_level = 0;
        model->input_stale = false;
        model->last_input_ms = now_ms;
        model->last_mouth_ms = now_ms;
        animate(model, now_ms);
        return true;
    }
    if (event.kind == FACE_EVENT_MOUTH_LEVEL) {
        int level = event.value;
        if (level < 0) {
            level = 0;
        } else if (level > FACE_MOUTH_LEVEL_MAX) {
            level = FACE_MOUTH_LEVEL_MAX;
        }
        model->mouth_level = (uint8_t)level;
        model->last_mouth_ms = now_ms;
    } else {
        static const face_state_t event_states[FACE_EVENT_COUNT] = {
            FACE_STATE_IDLE, FACE_STATE_LISTENING, FACE_STATE_THINKING,
            FACE_STATE_SPEAKING, FACE_STATE_ERROR, FACE_STATE_BLOCKED,
            /* Mouth/demo events are handled above and never use these slots. */
            FACE_STATE_IDLE, FACE_STATE_IDLE, FACE_STATE_IDLE,
            FACE_STATE_HAPPY, FACE_STATE_CONFUSED, FACE_STATE_SLEEP
        };
        const face_state_t previous_state = model->state;
        set_state(model, event_states[event.kind], now_ms);
        if (model->state == FACE_STATE_SPEAKING &&
            previous_state != FACE_STATE_SPEAKING) {
            model->last_mouth_ms = now_ms;
        }
        if (model->state != FACE_STATE_SPEAKING) {
            model->mouth_level = 0;
        }
    }
    model->last_input_ms = now_ms;
    model->input_stale = false;
    animate(model, now_ms);
    return true;
}

face_status_t face_model_status(const face_model_t *model)
{
    face_status_t status = { FACE_STATE_BLOCKED, false, true, false, 0 };
    if (model_valid(model)) {
        status.state = model->state;
        status.demo_mode = model->demo_mode;
        status.input_stale = model->input_stale;
        status.blink_closed = model->blink_closed;
        status.mouth_level = model->mouth_level;
    }
    return status;
}

const char *face_state_name(face_state_t state)
{
    static const char *const names[] = {
        "idle", "listening", "thinking", "speaking", "error", "blocked",
        "happy", "confused", "sleep"
    };
    return (unsigned)state < FACE_STATE_COUNT ? names[state] : "invalid";
}

typedef struct {
    uint16_t *pixels;
    int width;
    int height;
} canvas_t;

static void pixel(canvas_t *canvas, int x, int y, uint16_t color)
{
    if (x >= 0 && y >= 0 && x < canvas->width && y < canvas->height) {
        canvas->pixels[(size_t)y * (size_t)canvas->width + (size_t)x] = color;
    }
}

static void rectangle(canvas_t *canvas, int left, int top, int right,
                      int bottom, uint16_t color)
{
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > canvas->width) right = canvas->width;
    if (bottom > canvas->height) bottom = canvas->height;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            pixel(canvas, x, y, color);
        }
    }
}

static double projection_scale(void)
{
    return (double)STACKCHAN_SCALE_NUMERATOR / STACKCHAN_SCALE_DENOMINATOR;
}

/* Rasterize canonical coordinates at pixel centers. Canvas can antialias the
 * same shape's edge pixels; this RGB565 renderer uses binary coverage. */
static int project_x(double x)
{
    return (int)ceil(STACKCHAN_OFFSET_X + projection_scale() * x - 0.5);
}

static int project_y(double y)
{
    return (int)ceil(STACKCHAN_OFFSET_Y + projection_scale() * y - 0.5);
}

static void draw_eye(canvas_t *canvas, const face_model_t *model,
                     double cx, double cy, bool left)
{
    const double w = STACKCHAN_EYELID_WIDTH;
    const double h = STACKCHAN_EYELID_HEIGHT;
    const double viewport_x = cx - w / 2;
    const double viewport_y = cy - h / 2 + model->breath_offset;
    const double closed_h = h * (1.0 - (double)model->eye_open_step / STACKCHAN_EYE_OPEN_STEPS);
    for (int y = project_y(viewport_y); y < project_y(viewport_y + h); ++y) {
        for (int x = project_x(viewport_x); x < project_x(viewport_x + w); ++x) {
            const double nx = (x + 0.5 - STACKCHAN_OFFSET_X) / projection_scale();
            const double ny = (y + 0.5 - STACKCHAN_OFFSET_Y) / projection_scale();
            const double lx = nx - viewport_x;
            const double ly = ny - viewport_y;
            const double dx = nx - cx - 2.0 * model->gaze_x;
            const double dy = ny - cy - model->breath_offset - 2.0 * model->gaze_y;
            if (lx < 0 || lx >= w || ly < 0 || ly >= h ||
                dx * dx + dy * dy > STACKCHAN_EYE_RADIUS * STACKCHAN_EYE_RADIUS) continue;
            double mask_height = closed_h;
            if (model->state == FACE_STATE_ERROR) {
                /* SAD is upstream's slanted top eyelid, not a custom eye. */
                const double half = (h + closed_h) / 2;
                const double h1 = left ? half : closed_h;
                const double h2 = left ? closed_h : half;
                mask_height = h1 + (h2 - h1) * lx / w;
            } else if (model->state == FACE_STATE_SLEEP) {
                mask_height = h * 0.5 + closed_h * 0.5;
            } else if (model->state == FACE_STATE_HAPPY) {
                mask_height = closed_h * 0.6;
                if (ly >= h * 0.6) continue;
            }
            if (ly >= mask_height) pixel(canvas, x, y, COLOR_FACE);
        }
    }
}

static void draw_mouth(canvas_t *canvas, const face_model_t *model)
{
    const double open = model->state == FACE_STATE_SPEAKING
        ? (double)model->mouth_level / FACE_MOUTH_LEVEL_MAX : 0.0;
    const double width = STACKCHAN_MOUTH_MIN_WIDTH +
        (STACKCHAN_MOUTH_MAX_WIDTH - STACKCHAN_MOUTH_MIN_WIDTH) * (1.0 - open);
    const double height = STACKCHAN_MOUTH_MIN_HEIGHT +
        (STACKCHAN_MOUTH_MAX_HEIGHT - STACKCHAN_MOUTH_MIN_HEIGHT) * open;
    /* Upstream Port rounds its positive local x/y/w/h before projection. */
    const double x = STACKCHAN_MOUTH_X - STACKCHAN_MOUTH_MAX_WIDTH / 2.0 +
        floor((STACKCHAN_MOUTH_MAX_WIDTH - width) / 2.0 + 0.5);
    const double y = STACKCHAN_MOUTH_Y - STACKCHAN_MOUTH_MAX_HEIGHT / 2.0 +
        floor((STACKCHAN_MOUTH_MAX_HEIGHT - height) / 2.0 + 0.5) + model->breath_offset;
    rectangle(canvas, project_x(x), project_y(y),
              project_x(x + floor(width + 0.5)), project_y(y + floor(height + 0.5)), COLOR_FACE);
}

/* 5x7 uppercase glyphs, stored as top-to-bottom column bitmaps. */
static const uint8_t *glyph(char letter)
{
    static const char letters[] = "ABCDEFGHIKLMNOPRSTUY";
    static const uint8_t columns[][5] = {
        {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
        {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
        {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
        {0x3E,0x41,0x49,0x49,0x7A},
        {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
        {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
        {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
        {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
        {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
        {0x07,0x08,0x70,0x08,0x07}
    };
    for (size_t i = 0; i < sizeof(letters) - 1u; ++i) {
        if (letters[i] == letter) return columns[i];
    }
    return NULL;
}

static void text_centered(canvas_t *canvas, const char *text, int y,
                          int scale, uint16_t color)
{
    const size_t length = strlen(text);
    int x = (int)FACE_WIDTH / 2 - ((int)length * 6 - 1) * scale / 2;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t *columns = glyph(text[i]);
        if (columns != NULL) {
            for (int col = 0; col < 5; ++col) {
                for (int row = 0; row < 7; ++row) {
                    if ((columns[col] & (1u << row)) != 0) {
                        rectangle(canvas, x + col * scale, y + row * scale,
                                  x + (col + 1) * scale, y + (row + 1) * scale,
                                  color);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

bool face_render_rgb565(const face_model_t *model, uint16_t *pixels,
                       size_t pixel_count, size_t width, size_t height)
{
    if (!model_valid(model) || pixels == NULL || width == 0 || height == 0 ||
        width > INT_MAX || height > INT_MAX || width > SIZE_MAX / height ||
        width * height > pixel_count) {
        return false;
    }
    const size_t area = width * height;
    for (size_t i = 0; i < area; ++i) pixels[i] = COLOR_BACKGROUND;
    canvas_t canvas = { pixels, (int)width, (int)height };
    draw_eye(&canvas, model, STACKCHAN_LEFT_EYE_X, STACKCHAN_LEFT_EYE_Y, true);
    draw_eye(&canvas, model, STACKCHAN_RIGHT_EYE_X, STACKCHAN_RIGHT_EYE_Y, false);
    draw_mouth(&canvas, model);
    static const char *const labels[] = {
        "IDLE", "LISTEN", "THINK", "SPEAK", "ERROR", "BLOCKED",
        "HAPPY", "CONFUSED", "SLEEP"
    };
    text_centered(&canvas, model->input_stale ? "STALE" : labels[model->state],
                  213, 1, COLOR_FACE);
    if (model->demo_mode) {
        text_centered(&canvas, "DEMO", 9, 2, COLOR_FACE);
    }
    return true;
}
