#include "face.h"

#include <limits.h>
#include <string.h>

enum {
    COLOR_BACKGROUND = 0x0842,
    COLOR_FACE = 0x9FFF,
    COLOR_LISTENING = 0x07FF,
    COLOR_THINKING = 0xBCDF,
    COLOR_ERROR = 0xF986,
    COLOR_BLOCKED = 0xFDC0,
    COLOR_TEXT = 0x94B2
};

static bool model_valid(const face_model_t *model)
{
    return model != NULL && (unsigned)model->state < FACE_STATE_COUNT &&
           model->mouth_level <= FACE_MOUTH_LEVEL_MAX &&
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
    face_model_init_seeded(model, demo_mode, now_ms, UINT32_C(0x4E454D4F));
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
}

static void animate(face_model_t *model, uint64_t now_ms)
{
    model->animation_ms = (uint32_t)((now_ms - model->state_since_ms) % 60000u);
    const uint64_t elapsed = now_ms - model->blink_anchor_ms;
    const unsigned phase = (unsigned)(elapsed % FACE_BLINK_PERIOD_MS);
    /* One seeded window per slot, computed directly for arbitrarily sparse
     * ticks. The entire 140ms window fits within its slot. */
    uint64_t hash = elapsed / FACE_BLINK_PERIOD_MS + model->blink_seed;
    hash ^= hash >> 30;
    hash *= UINT64_C(0xBF58476D1CE4E5B9);
    hash ^= hash >> 27;
    hash *= UINT64_C(0x94D049BB133111EB);
    hash ^= hash >> 31;
    const unsigned window_range = FACE_BLINK_PERIOD_MS -
        FACE_BLINK_DURATION_MS - FACE_BLINK_EARLIEST_MS + 1u;
    const unsigned start = FACE_BLINK_EARLIEST_MS + (unsigned)(hash % window_range);
    model->blink_closed = model->state == FACE_STATE_SLEEP ||
        (phase >= start && phase - start < FACE_BLINK_DURATION_MS);
}

bool face_model_tick(face_model_t *model, uint64_t now_ms)
{
    if (!model_valid(model) || now_ms < model->last_tick_ms) {
        return false;
    }
    const face_status_t previous = face_model_status(model);
    const uint32_t previous_animation = model->animation_ms;
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
           previous_animation != model->animation_ms;
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

static void ellipse(canvas_t *canvas, int cx, int cy, int rx, int ry,
                    uint16_t color)
{
    const int64_t rx2 = (int64_t)rx * rx;
    const int64_t ry2 = (int64_t)ry * ry;
    for (int y = -ry; y <= ry; ++y) {
        for (int x = -rx; x <= rx; ++x) {
            if ((int64_t)x * x * ry2 + (int64_t)y * y * rx2 <= rx2 * ry2) {
                pixel(canvas, cx + x, cy + y, color);
            }
        }
    }
}

static void line(canvas_t *canvas, int x0, int y0, int x1, int y1,
                 uint16_t color)
{
    const int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    const int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        rectangle(canvas, x0 - 1, y0 - 1, x0 + 2, y0 + 2, color);
        if (x0 == x1 && y0 == y1) break;
        const int twice_error = 2 * error;
        if (twice_error >= dy) { error += dy; x0 += sx; }
        if (twice_error <= dx) { error += dx; y0 += sy; }
    }
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
    uint16_t color = COLOR_FACE;
    if (model->state == FACE_STATE_LISTENING) color = COLOR_LISTENING;
    if (model->state == FACE_STATE_THINKING) color = COLOR_THINKING;
    if (model->state == FACE_STATE_ERROR) color = COLOR_ERROR;
    if (model->state == FACE_STATE_BLOCKED) color = COLOR_BLOCKED;
    const int bob = model->state == FACE_STATE_IDLE
        ? (int)triangle(model->animation_ms, 2400u, 4u) - 2 : 0;
    int gaze = 0;
    if (model->state == FACE_STATE_THINKING || model->state == FACE_STATE_CONFUSED) {
        gaze = (int)triangle(model->animation_ms, 1800u, 8u) - 4;
    } else if (model->state != FACE_STATE_ERROR &&
               model->state != FACE_STATE_BLOCKED && model->state != FACE_STATE_SLEEP) {
        gaze = (int)triangle(model->animation_ms, 2800u, 4u) - 2;
    }
    const int ry = model->state == FACE_STATE_LISTENING ? 35 : 30;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const int cx = (eye == 0 ? 75 : 165) + gaze;
        const int cy = 96 + bob;
        if (model->state == FACE_STATE_ERROR) {
            line(&canvas, cx - 16, cy - 18, cx + 16, cy + 18, color);
            line(&canvas, cx + 16, cy - 18, cx - 16, cy + 18, color);
        } else if (model->state == FACE_STATE_BLOCKED) {
            rectangle(&canvas, cx - 25, cy - 5, cx + 26, cy + 6, color);
        } else if (model->blink_closed) {
            rectangle(&canvas, cx - 24, cy - 2, cx + 25, cy + 3, color);
        } else if (model->state == FACE_STATE_HAPPY) {
            line(&canvas, cx - 20, cy + 6, cx, cy - 10, color);
            line(&canvas, cx, cy - 10, cx + 20, cy + 6, color);
        } else {
            const int eye_height = model->state == FACE_STATE_CONFUSED
                ? (eye == 0 ? 23 : 34) : ry;
            ellipse(&canvas, cx, cy, 24, eye_height, color);
            ellipse(&canvas, cx - 7, cy - 12, 4, 6, 0xFFFF);
        }
    }
    if (model->state == FACE_STATE_SPEAKING) {
        const int mouth_height = 3 + model->mouth_level * 20 / FACE_MOUTH_LEVEL_MAX;
        ellipse(&canvas, 120, 169, 23, mouth_height, color);
        if (mouth_height > 5) {
            ellipse(&canvas, 120, 168, 18, mouth_height - 4, COLOR_BACKGROUND);
        }
    } else if (model->state == FACE_STATE_LISTENING) {
        ellipse(&canvas, 120, 169, 10, 13, color);
        ellipse(&canvas, 120, 169, 6, 9, COLOR_BACKGROUND);
    } else if (model->state == FACE_STATE_THINKING ||
               model->state == FACE_STATE_BLOCKED || model->state == FACE_STATE_SLEEP) {
        rectangle(&canvas, 103, 168, 138, 172, color);
    } else if (model->state == FACE_STATE_HAPPY) {
        ellipse(&canvas, 120, 163, 27, 20, color);
        rectangle(&canvas, 92, 140, 149, 162, COLOR_BACKGROUND);
    } else if (model->state == FACE_STATE_CONFUSED) {
        line(&canvas, 101, 174, 139, 163, color);
    } else if (model->state == FACE_STATE_ERROR) {
        line(&canvas, 99, 175, 110, 166, color);
        line(&canvas, 110, 166, 121, 175, color);
        line(&canvas, 121, 175, 132, 166, color);
        line(&canvas, 132, 166, 143, 175, color);
    } else {
        const int xs[] = { 96, 108, 120, 132, 144 };
        const int ys[] = { 164, 170, 172, 170, 164 };
        for (unsigned i = 0; i < 4; ++i) {
            line(&canvas, xs[i], ys[i] + bob, xs[i + 1], ys[i + 1] + bob, color);
        }
    }
    static const char *const labels[] = {
        "IDLE", "LISTEN", "THINK", "SPEAK", "ERROR", "BLOCKED",
        "HAPPY", "CONFUSED", "SLEEP"
    };
    text_centered(&canvas, model->input_stale ? "STALE" : labels[model->state],
                  213, 1, COLOR_TEXT);
    if (model->demo_mode) {
        text_centered(&canvas, "DEMO", 17, 2, COLOR_BLOCKED);
    }
    return true;
}
