#include "face.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FRAME_PIXELS (FACE_WIDTH * FACE_HEIGHT)

static const face_event_kind_t STATE_EVENTS[FACE_STATE_COUNT] = {
    FACE_EVENT_IDLE, FACE_EVENT_LISTENING, FACE_EVENT_THINKING,
    FACE_EVENT_SPEAKING, FACE_EVENT_ERROR, FACE_EVENT_BLOCKED,
    FACE_EVENT_HAPPY, FACE_EVENT_CONFUSED, FACE_EVENT_SLEEP
};

static bool event(face_model_t *model, face_event_kind_t kind, int value,
                  uint64_t now_ms)
{
    const face_event_t input = { kind, value };
    return face_model_handle_event(model, input, now_ms);
}

static uint64_t frame_hash(const uint16_t *pixels, size_t count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < count; ++i) {
        hash ^= pixels[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Independent coverage oracle: invert the documented projection at each
 * pixel center, then test the pinned native circles, masks and Port rectangle.
 * Labels are deliberately excluded from the 240x180 canonical face region. */
static bool reference_eye(face_state_t state, unsigned open_step, bool left,
                          double x, double y, double gaze_x, double gaze_y)
{
    const double center_x = left ? 90.0 : 230.0;
    const double center_y = left ? 93.0 : 96.0;
    const double local_x = x - center_x + 12.0;
    const double local_y = y - center_y + 12.0;
    if (local_x < 0 || local_x >= 24 || local_y < 0 || local_y >= 24) return false;
    const double dx = x - center_x - 2.0 * gaze_x;
    const double dy = y - center_y - 2.0 * gaze_y;
    if (dx * dx + dy * dy > 64.0) return false;
    const double covered_height = 24.0 * (1.0 - open_step / 12.0);
    if (state == FACE_STATE_ERROR) {
        const double outer = (24.0 + covered_height) / 2.0;
        const double left_height = left ? outer : covered_height;
        const double right_height = left ? covered_height : outer;
        return local_y >= left_height + (right_height - left_height) * local_x / 24.0;
    }
    if (state == FACE_STATE_HAPPY) {
        return local_y >= covered_height * 0.6 && local_y < 14.4;
    }
    if (state == FACE_STATE_SLEEP) return local_y >= 12.0 + covered_height / 2.0;
    return local_y >= covered_height;
}

static bool reference_pixel(const face_model_t *model, size_t x, size_t y)
{
    const double native_x = (x + 0.5) / 0.75;
    const double native_y = (y + 0.5 - 30.0) / 0.75 - model->breath_offset;
    if (reference_eye(model->state, model->eye_open_step, true, native_x, native_y,
                      model->gaze_x, model->gaze_y) ||
        reference_eye(model->state, model->eye_open_step, false, native_x, native_y,
                      model->gaze_x, model->gaze_y)) return true;
    const double open = model->state == FACE_STATE_SPEAKING ? model->mouth_level / 100.0 : 0.0;
    const double width = 90.0 - 40.0 * open;
    const double height = 8.0 + 50.0 * open;
    const double left = 115.0 + floor((90.0 - width) / 2.0 + 0.5);
    const double top = 119.0 + floor((58.0 - height) / 2.0 + 0.5);
    return native_x >= left && native_x < left + floor(width + 0.5) &&
           native_y >= top && native_y < top + floor(height + 0.5);
}

static void assert_canonical_frame(const face_model_t *model, const uint16_t *frame)
{
    for (size_t y = 30; y < 210; ++y) {
        for (size_t x = 0; x < FACE_WIDTH; ++x) {
            const uint16_t expected = reference_pixel(model, x, y) ? 0xFFFF : 0x0000;
            const uint16_t actual = frame[y * FACE_WIDTH + x];
            if (actual != expected) {
                fprintf(stderr, "canonical pixel mismatch: state=%s open=%u mouth=%u (%zu,%zu) expected=%04x actual=%04x\n",
                        face_state_name(model->state), model->eye_open_step,
                        model->mouth_level, x, y, (unsigned)expected, (unsigned)actual);
                assert(actual == expected);
            }
        }
    }
    for (size_t index = 0; index < FRAME_PIXELS; ++index) {
        assert(frame[index] == 0x0000 || frame[index] == 0xFFFF);
    }
}

typedef struct {
    int left, top, right, bottom;
    size_t count;
} bounds_t;

static bounds_t white_bounds(const uint16_t *frame, int x0, int y0, int x1, int y1)
{
    bounds_t bounds = {INT_MAX, INT_MAX, -1, -1, 0};
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (frame[(size_t)y * FACE_WIDTH + (size_t)x] == 0xFFFF) {
                if (x < bounds.left) bounds.left = x;
                if (x > bounds.right) bounds.right = x;
                if (y < bounds.top) bounds.top = y;
                if (y > bounds.bottom) bounds.bottom = y;
                ++bounds.count;
            }
        }
    }
    return bounds;
}

static void assert_bounds(bounds_t actual, int left, int top, int right, int bottom)
{
    assert(actual.count > 0);
    assert(actual.left == left && actual.top == top && actual.right == right && actual.bottom == bottom);
}

static void test_transitions_and_status(void)
{
    face_model_t model;
    face_model_init(&model, false, 100);
    face_status_t status = face_model_status(&model);
    assert(status.state == FACE_STATE_IDLE);
    assert(!status.demo_mode && !status.input_stale && status.mouth_level == 0);
    const face_event_kind_t kinds[] = {
        FACE_EVENT_LISTENING, FACE_EVENT_THINKING, FACE_EVENT_SPEAKING,
        FACE_EVENT_ERROR, FACE_EVENT_BLOCKED, FACE_EVENT_HAPPY,
        FACE_EVENT_CONFUSED, FACE_EVENT_SLEEP, FACE_EVENT_IDLE
    };
    const face_state_t states[] = {
        FACE_STATE_LISTENING, FACE_STATE_THINKING, FACE_STATE_SPEAKING,
        FACE_STATE_ERROR, FACE_STATE_BLOCKED, FACE_STATE_HAPPY,
        FACE_STATE_CONFUSED, FACE_STATE_SLEEP, FACE_STATE_IDLE
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        const uint64_t now_ms = 200 + i * 100;
        assert(event(&model, kinds[i], 0, now_ms));
        assert(face_model_status(&model).state == states[i]);
        assert(model.state_since_ms == now_ms);
        assert(strcmp(face_state_name(states[i]), "invalid") != 0);
    }
    assert(strcmp(face_state_name((face_state_t)-1), "invalid") == 0);
    assert(strcmp(face_state_name(FACE_STATE_COUNT), "invalid") == 0);
    assert(face_model_status(NULL).state == FACE_STATE_BLOCKED);
    face_model_init(NULL, false, 0);
}

static void test_invalid_inputs_are_atomic(void)
{
    face_model_t model;
    face_model_init(&model, false, 0);
    assert(event(&model, FACE_EVENT_LISTENING, 0, 10));
    face_model_t saved = model;
    assert(!event(&model, (face_event_kind_t)-1, 0, 20));
    assert(memcmp(&model, &saved, sizeof(model)) == 0);
    assert(!event(&model, FACE_EVENT_COUNT, 0, 20));
    assert(memcmp(&model, &saved, sizeof(model)) == 0);
    assert(!event(&model, FACE_EVENT_MOUTH_LEVEL, 50, 20));
    assert(memcmp(&model, &saved, sizeof(model)) == 0);
    assert(!event(&model, FACE_EVENT_ERROR, 0, 9));
    assert(!face_model_tick(&model, 9));
    assert(memcmp(&model, &saved, sizeof(model)) == 0);
    assert(!face_model_handle_event(NULL, (face_event_t){FACE_EVENT_IDLE, 0}, 0));
    assert(!face_model_tick(NULL, 0));
    model.state = (face_state_t)INT_MAX;
    assert(!face_model_tick(&model, 20));
    assert(!event(&model, FACE_EVENT_IDLE, 0, 20));
}

static void test_mouth_clamp_and_timeout(void)
{
    face_model_t model;
    face_model_init(&model, false, 0);
    assert(event(&model, FACE_EVENT_SPEAKING, 0, 10));
    assert(event(&model, FACE_EVENT_MOUTH_LEVEL, INT_MAX, 20));
    assert(model.mouth_level == FACE_MOUTH_LEVEL_MAX);
    assert(event(&model, FACE_EVENT_MOUTH_LEVEL, INT_MIN, 21));
    assert(model.mouth_level == 0);
    assert(event(&model, FACE_EVENT_MOUTH_LEVEL, 47, 22));
    assert(model.mouth_level == 47);
    /* A speaking heartbeat does not claim a fresh audio-level sample. */
    assert(event(&model, FACE_EVENT_SPEAKING, 0, 23 + FACE_MOUTH_TIMEOUT_MS / 2));
    assert(model.state_since_ms == 10);
    (void)face_model_tick(&model, 22 + FACE_MOUTH_TIMEOUT_MS - 1);
    assert(model.mouth_level == 47);
    (void)face_model_tick(&model, 22 + FACE_MOUTH_TIMEOUT_MS);
    assert(model.state == FACE_STATE_SPEAKING && model.mouth_level == 0);
    assert(event(&model, FACE_EVENT_MOUTH_LEVEL, 75, 800));
    assert(event(&model, FACE_EVENT_ERROR, 0, 801));
    assert(model.mouth_level == 0);
    assert(!event(&model, FACE_EVENT_MOUTH_LEVEL, 75, 802));
}

static void test_input_timeout_and_recovery(void)
{
    const face_event_kind_t active[] = {
        FACE_EVENT_LISTENING, FACE_EVENT_THINKING, FACE_EVENT_SPEAKING
    };
    for (size_t i = 0; i < sizeof(active) / sizeof(active[0]); ++i) {
        face_model_t model;
        face_model_init(&model, false, 0);
        assert(event(&model, active[i], 0, 100));
        (void)face_model_tick(&model, 100 + FACE_INPUT_TIMEOUT_MS - 1);
        assert(model.state != FACE_STATE_IDLE && !model.input_stale);
        (void)face_model_tick(&model, 100 + FACE_INPUT_TIMEOUT_MS);
        assert(model.state == FACE_STATE_IDLE && model.input_stale);
        assert(model.mouth_level == 0);
        assert(event(&model, FACE_EVENT_LISTENING, 0, 5200));
        assert(model.state == FACE_STATE_LISTENING && !model.input_stale);
    }
    face_model_t model;
    face_model_init(&model, false, 0);
    assert(event(&model, FACE_EVENT_SPEAKING, 0, 100));
    face_model_t saved = model;
    assert(!event(&model, FACE_EVENT_MOUTH_LEVEL, 50,
                  100 + FACE_INPUT_TIMEOUT_MS));
    assert(memcmp(&model, &saved, sizeof(model)) == 0);
    /* Error and blocked need an explicit recovery event. */
    assert(event(&model, FACE_EVENT_ERROR, 0, 200));
    (void)face_model_tick(&model, 20000);
    assert(model.state == FACE_STATE_ERROR && !model.input_stale);
    assert(event(&model, FACE_EVENT_BLOCKED, 0, 20001));
    (void)face_model_tick(&model, 40000);
    assert(model.state == FACE_STATE_BLOCKED && !model.input_stale);
}

static void test_blink_and_clock_boundaries(void)
{
    face_model_t model;
    face_model_init_seeded(&model, false, 500, 37);
    assert(!model.blink_closed);
    for (uint64_t elapsed = 0; elapsed < 12000; elapsed += 17) {
        (void)face_model_tick(&model, 500 + elapsed);
        const face_motion_t motion = face_motion_sample(37, elapsed);
        assert(model.eye_open_step == motion.eye_open_step);
        assert(model.blink_closed == (motion.eye_open_step <= 2));
        assert(model.gaze_x == motion.gaze_x && model.gaze_y == motion.gaze_y);
        assert(model.breath_offset == motion.breath_offset);
    }
    assert(!face_model_tick(&model, model.last_tick_ms));
    /* Crossing the 32-bit millisecond boundary must not wrap animations. */
    const uint64_t start = UINT64_C(0xFFFFFFFF) - 100;
    face_model_init(&model, false, start);
    assert(event(&model, FACE_EVENT_LISTENING, 0, start + 50));
    (void)face_model_tick(&model, start + 5050);
    assert(model.state == FACE_STATE_IDLE && model.input_stale);
    /* Near UINT64_MAX, timeout arithmetic remains bounded and subtraction-safe. */
    face_model_init(&model, false, UINT64_MAX - 10000);
    assert(event(&model, FACE_EVENT_THINKING, 0, UINT64_MAX - 6000));
    (void)face_model_tick(&model, UINT64_MAX);
    assert(model.state == FACE_STATE_IDLE);
    assert(model.state_since_ms == UINT64_MAX - 1000);
    const face_model_t saved = model;
    assert(!face_model_tick(&model, UINT64_MAX - 1));
    assert(memcmp(&model, &saved, sizeof(model)) == 0);
    face_model_init(&model, false, 0);
    assert(event(&model, FACE_EVENT_SPEAKING, 0, 1));
    assert(event(&model, FACE_EVENT_MOUTH_LEVEL, 99, 2));
    assert(event(&model, FACE_EVENT_SLEEP, 0, 3));
    assert(model.blink_closed && model.mouth_level == 0);
    (void)face_model_tick(&model, 10000);
    assert(model.state == FACE_STATE_SLEEP && model.blink_closed);
    assert(event(&model, FACE_EVENT_IDLE, 0, 10001));
    assert(model.state == FACE_STATE_IDLE);
}

static void assert_motion_bounds(face_motion_t motion)
{
    assert(isfinite(motion.eye_open) && motion.eye_open >= 0.2 && motion.eye_open <= 1.0);
    assert(motion.eye_open_step >= 2 && motion.eye_open_step <= STACKCHAN_EYE_OPEN_STEPS);
    assert(motion.eye_open_step == (unsigned)floor(motion.eye_open * 12.0 + 0.5));
    assert(isfinite(motion.gaze_x) && isfinite(motion.gaze_y));
    assert(motion.breath_offset >= -6 && motion.breath_offset <= 6);
    assert(motion.blink_slot < STACKCHAN_ANIMATION_SLOTS);
    assert(motion.gaze_slot < STACKCHAN_ANIMATION_SLOTS);
    assert(motion.blink_open_ms >= STACKCHAN_BLINK_OPEN_MIN_MS);
    assert(motion.blink_open_ms < STACKCHAN_BLINK_OPEN_MAX_MS);
    assert(motion.blink_transition_ms >= STACKCHAN_BLINK_TRANSITION_MIN_MS);
    assert(motion.blink_transition_ms < STACKCHAN_BLINK_TRANSITION_MAX_MS);
    assert(motion.blink_cycle_ms >= STACKCHAN_ANIMATION_SLOTS *
           (STACKCHAN_BLINK_OPEN_MIN_MS + STACKCHAN_BLINK_TRANSITION_MIN_MS));
    assert(motion.blink_cycle_ms < STACKCHAN_ANIMATION_SLOTS *
           (STACKCHAN_BLINK_OPEN_MAX_MS + STACKCHAN_BLINK_TRANSITION_MAX_MS));
    assert(motion.gaze_cycle_ms >= STACKCHAN_ANIMATION_SLOTS * STACKCHAN_GAZE_INTERVAL_MIN_MS);
    assert(motion.gaze_cycle_ms < STACKCHAN_ANIMATION_SLOTS * STACKCHAN_GAZE_INTERVAL_MAX_MS);
}

/* Find a small, equivalent time on the 33ms sampling grid. Reducing an
 * unquantized timestamp modulo a raw schedule would lose the tick phase. */
static uint64_t reduced_tick(uint64_t quantized, uint64_t cycle)
{
    uint64_t reduced = quantized % cycle;
    for (unsigned attempt = 0; attempt < STACKCHAN_MOTION_TICK_MS; ++attempt) {
        if (reduced % STACKCHAN_MOTION_TICK_MS == 0) return reduced;
        reduced += cycle;
    }
    assert(false);
    return 0;
}

static void test_motion_intervals_curve_and_large_time(void)
{
    const uint32_t seeds[] = {0, 1, STACKCHAN_ANIMATION_SEED, UINT32_MAX};
    bool different_seeds = false;
    bool saw_closed = false;
    bool saw_partial = false;
    for (size_t seed_index = 0; seed_index < sizeof(seeds) / sizeof(seeds[0]); ++seed_index) {
        const uint32_t seed = seeds[seed_index];
        const face_motion_t initial = face_motion_sample(seed, 0);
        assert_motion_bounds(initial);
        assert(initial.eye_open == 1 && initial.eye_open_step == 12);
        assert(initial.gaze_x == 0 && initial.gaze_y == 0 && initial.breath_offset == 0);
        if (seed_index > 0 && initial.blink_open_ms != face_motion_sample(seeds[0], 0).blink_open_ms) {
            different_seeds = true;
        }
        uint64_t start = 0;
        unsigned first_open_ms = initial.blink_open_ms;
        bool intervals_vary = false;
        for (unsigned slot = 0; slot < STACKCHAN_ANIMATION_SLOTS; ++slot) {
            const uint64_t first_tick = start +
                (STACKCHAN_MOTION_TICK_MS - start % STACKCHAN_MOTION_TICK_MS) % STACKCHAN_MOTION_TICK_MS;
            const face_motion_t at_start = face_motion_sample(seed, first_tick);
            assert_motion_bounds(at_start);
            assert(at_start.blink_slot == slot && at_start.eye_open == 1.0);
            if (at_start.blink_open_ms != first_open_ms) intervals_vary = true;
            const uint64_t end = start + at_start.blink_open_ms + at_start.blink_transition_ms;
            for (uint64_t tick = first_tick; tick < end; tick += STACKCHAN_MOTION_TICK_MS) {
                const face_motion_t sample = face_motion_sample(seed, tick);
                assert_motion_bounds(sample);
                assert(sample.blink_slot == slot);
                double expected_open = 1.0;
                if (tick - start >= at_start.blink_open_ms) {
                    const double phase = (double)(tick - start - at_start.blink_open_ms) /
                        at_start.blink_transition_ms;
                    expected_open = phase < 0.25 ? 1.0 - 3.2 * phase :
                        0.2 + 0.8 * 16.0 * (phase - 0.25) * (phase - 0.25) / 9.0;
                }
                assert(fabs(sample.eye_open - expected_open) < 1e-12);
                saw_closed |= sample.eye_open_step <= 2;
                saw_partial |= sample.eye_open_step > 2 && sample.eye_open_step < 12;
            }
            start = end;
        }
        assert(start == initial.blink_cycle_ms && intervals_vary);

        face_motion_t previous = initial;
        uint64_t gaze_started = 0;
        unsigned gaze_changes = 0;
        bool axes_differ = false;
        for (uint64_t tick = STACKCHAN_MOTION_TICK_MS; tick < initial.gaze_cycle_ms;
             tick += STACKCHAN_MOTION_TICK_MS) {
            const face_motion_t sample = face_motion_sample(seed, tick);
            if (sample.gaze_slot == previous.gaze_slot) {
                assert(sample.gaze_x == previous.gaze_x && sample.gaze_y == previous.gaze_y);
            } else {
                assert(sample.gaze_slot == previous.gaze_slot + 1);
                const uint64_t duration = tick - gaze_started;
                assert(duration >= STACKCHAN_GAZE_INTERVAL_MIN_MS - STACKCHAN_MOTION_TICK_MS);
                assert(duration < STACKCHAN_GAZE_INTERVAL_MAX_MS + STACKCHAN_MOTION_TICK_MS);
                gaze_started = tick;
                ++gaze_changes;
                axes_differ |= sample.gaze_x != sample.gaze_y;
            }
            previous = sample;
        }
        assert(gaze_changes == STACKCHAN_ANIMATION_SLOTS - 1 && axes_differ);
        assert(face_motion_sample(seed, 1500).breath_offset == 6);
        /* 16500 is an exact trough on the 33ms grid; 4500 is rounded to
         * 4488, where upstream ceil quantization gives a -5 pixel offset. */
        assert(face_motion_sample(seed, 16500).breath_offset == -6);
        for (uint64_t tick = 0; tick < 6000; tick += 33) {
            assert(face_motion_sample(seed, tick).breath_offset ==
                   face_motion_sample(seed, tick + UINT64_C(66000)).breath_offset);
        }

        const uint64_t times[] = {
            UINT64_C(0xFFFFFFFF), UINT64_C(0x100000000), UINT64_C(1) << 63,
            UINT64_MAX - 1, UINT64_MAX
        };
        for (size_t index = 0; index < sizeof(times) / sizeof(times[0]); ++index) {
            const face_motion_t large = face_motion_sample(seed, times[index]);
            const uint64_t quantized = times[index] - times[index] % STACKCHAN_MOTION_TICK_MS;
            assert_motion_bounds(large);
            const face_motion_t blink = face_motion_sample(seed, reduced_tick(quantized, large.blink_cycle_ms));
            const face_motion_t gaze = face_motion_sample(seed, reduced_tick(quantized, large.gaze_cycle_ms));
            const face_motion_t breath = face_motion_sample(seed, reduced_tick(quantized, STACKCHAN_BREATH_PERIOD_MS));
            assert(large.eye_open == blink.eye_open && large.eye_open_step == blink.eye_open_step);
            assert(large.blink_slot == blink.blink_slot);
            assert(large.gaze_slot == gaze.gaze_slot);
            assert(large.gaze_x == gaze.gaze_x && large.gaze_y == gaze.gaze_y);
            assert(large.breath_offset == breath.breath_offset);
        }
    }
    assert(different_seeds && saw_closed && saw_partial);
}

static void assert_same_motion(const face_model_t *left, const face_model_t *right)
{
    assert(left->eye_open_step == right->eye_open_step);
    assert(left->blink_closed == right->blink_closed);
    assert(left->breath_offset == right->breath_offset);
    assert(left->gaze_x == right->gaze_x && left->gaze_y == right->gaze_y);
    assert(left->animation_ms == right->animation_ms);
}

static void test_motion_clock_is_independent_of_state(void)
{
    face_model_t active;
    face_model_t idle;
    face_model_init_seeded(&active, false, 100, 17);
    face_model_init_seeded(&idle, false, 100, 17);
    const face_event_kind_t states[] = {
        FACE_EVENT_LISTENING, FACE_EVENT_THINKING, FACE_EVENT_SPEAKING,
        FACE_EVENT_ERROR, FACE_EVENT_BLOCKED, FACE_EVENT_HAPPY,
        FACE_EVENT_CONFUSED, FACE_EVENT_IDLE, FACE_EVENT_DEMO_ENABLE,
        FACE_EVENT_DEMO_DISABLE
    };
    for (size_t index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
        const uint64_t now_ms = 500 + index * 400;
        (void)face_model_tick(&idle, now_ms);
        assert(event(&active, states[index], 0, now_ms));
        assert(active.blink_anchor_ms == 100);
        assert_same_motion(&active, &idle);
    }
    face_model_t near_limit;
    face_model_init_seeded(&idle, false, 0, 17);
    face_model_init_seeded(&near_limit, false, UINT64_MAX - 10000, 17);
    (void)face_model_tick(&idle, 10000);
    (void)face_model_tick(&near_limit, UINT64_MAX);
    assert_same_motion(&idle, &near_limit);
}

static void test_demo_is_explicit_and_deterministic(void)
{
    face_model_t model;
    face_model_init(&model, true, 100);
    assert(model.demo_mode && model.state == FACE_STATE_IDLE);
    face_model_t saved = model;
    assert(!event(&model, FACE_EVENT_LISTENING, 0, 101));
    assert(memcmp(&model, &saved, sizeof(model)) == 0);
    const face_state_t expected[] = {
        FACE_STATE_IDLE, FACE_STATE_LISTENING,
        FACE_STATE_THINKING, FACE_STATE_SPEAKING, FACE_STATE_IDLE
    };
    for (unsigned i = 0; i < 5; ++i) {
        (void)face_model_tick(&model, 100 + i * 3000u);
        const face_status_t status = face_model_status(&model);
        assert(status.state == expected[i] && status.demo_mode);
        assert(!status.input_stale);
    }
    face_model_init(&model, true, 100);
    (void)face_model_tick(&model, 9225);
    assert(model.state == FACE_STATE_SPEAKING && model.mouth_level == 50);
    (void)face_model_tick(&model, 9350);
    assert(model.mouth_level == 100);
    assert(event(&model, FACE_EVENT_DEMO_DISABLE, 0, 9351));
    assert(!model.demo_mode && model.state == FACE_STATE_IDLE && model.mouth_level == 0);
    assert(event(&model, FACE_EVENT_LISTENING, 0, 9352));
    assert(event(&model, FACE_EVENT_DEMO_ENABLE, 0, 9353));
    assert(model.demo_mode && model.state == FACE_STATE_IDLE);

    face_model_t sparse;
    face_model_t frequent;
    face_model_init(&sparse, true, 100);
    face_model_init(&frequent, true, 100);
    for (uint64_t now_ms = 101; now_ms <= 25000; now_ms += 17) {
        (void)face_model_tick(&frequent, now_ms);
    }
    (void)face_model_tick(&sparse, 25125);
    (void)face_model_tick(&frequent, 25125);
    assert(memcmp(&sparse, &frequent, sizeof(sparse)) == 0);

    /* Real-input expiry also uses the actual deadline rather than tick cadence. */
    face_model_init(&sparse, false, 100);
    face_model_init(&frequent, false, 100);
    assert(event(&sparse, FACE_EVENT_THINKING, 0, 200));
    assert(event(&frequent, FACE_EVENT_THINKING, 0, 200));
    (void)face_model_tick(&frequent, 5200);
    (void)face_model_tick(&frequent, 9000);
    (void)face_model_tick(&sparse, 9000);
    assert(memcmp(&sparse, &frequent, sizeof(sparse)) == 0);
}

static void test_renderer_clips_and_rejects_short_buffers(void)
{
    face_model_t model;
    face_model_init(&model, true, 0);
    const size_t dimensions[][2] = {
        {1, 1}, {7, 11}, {73, 97}, {239, 239}, {240, 240}, {257, 263}
    };
    for (size_t d = 0; d < sizeof(dimensions) / sizeof(dimensions[0]); ++d) {
        const size_t width = dimensions[d][0];
        const size_t height = dimensions[d][1];
        const size_t area = width * height;
        uint16_t *buffer = malloc((area + 4) * sizeof(*buffer));
        assert(buffer != NULL);
        for (unsigned state = 0; state < FACE_STATE_COUNT; ++state) {
            face_model_init(&model, false, 0);
            assert(event(&model, STATE_EVENTS[state], 0, 1));
            if (state == FACE_STATE_SPEAKING) {
                assert(event(&model, FACE_EVENT_MOUTH_LEVEL, 100, 2));
            }
            for (size_t i = 0; i < area + 4; ++i) buffer[i] = 0x55AA;
            assert(face_render_rgb565(&model, buffer + 2, area, width, height));
            assert(buffer[0] == 0x55AA && buffer[1] == 0x55AA);
            assert(buffer[area + 2] == 0x55AA && buffer[area + 3] == 0x55AA);
            /* Rendering overwrites every pixel, including larger-frame margins. */
            for (size_t i = 2; i < area + 2; ++i) assert(buffer[i] != 0x55AA);
        }
        free(buffer);
    }
    uint16_t small[4] = { 1, 2, 3, 4 };
    const uint16_t expected[4] = { 1, 2, 3, 4 };
    assert(!face_render_rgb565(&model, small, 3, 2, 2));
    assert(!face_render_rgb565(&model, small, 4, 0, 2));
    assert(!face_render_rgb565(&model, small, SIZE_MAX, SIZE_MAX, 2));
    assert(!face_render_rgb565(&model, small, SIZE_MAX, 2, SIZE_MAX));
    assert(!face_render_rgb565(&model, NULL, 4, 2, 2));
    assert(!face_render_rgb565(NULL, small, 4, 2, 2));
    assert(memcmp(small, expected, sizeof(small)) == 0);
    model.mouth_level = UINT8_MAX;
    assert(!face_render_rgb565(&model, small, 4, 2, 2));
    assert(memcmp(small, expected, sizeof(small)) == 0);
    face_model_init(&model, false, 0);
    model.eye_open_step = 13;
    assert(!face_render_rgb565(&model, small, 4, 2, 2));
    assert(memcmp(small, expected, sizeof(small)) == 0);
    face_model_init(&model, false, 0);
    model.breath_offset = INT8_MAX;
    assert(!face_render_rgb565(&model, small, 4, 2, 2));
    assert(memcmp(small, expected, sizeof(small)) == 0);
    face_model_init(&model, false, 0);
    model.gaze_x = NAN;
    assert(!face_render_rgb565(&model, small, 4, 2, 2));
    assert(memcmp(small, expected, sizeof(small)) == 0);
    face_model_init(&model, false, 0);
    model.gaze_y = INFINITY;
    assert(!face_render_rgb565(&model, small, 4, 2, 2));
    assert(memcmp(small, expected, sizeof(small)) == 0);
}

static void test_canonical_raster_masks_and_motion(void)
{
    uint16_t *frame = malloc(FRAME_PIXELS * sizeof(*frame));
    assert(frame != NULL);
    face_model_t model;
    assert(STACKCHAN_SOURCE_WIDTH == 320 && STACKCHAN_SOURCE_HEIGHT == 240);
    assert(STACKCHAN_TARGET_WIDTH == 240 && STACKCHAN_TARGET_HEIGHT == 240);
    assert(STACKCHAN_SCALE_NUMERATOR == 3 && STACKCHAN_SCALE_DENOMINATOR == 4);
    assert(STACKCHAN_OFFSET_X == 0 && STACKCHAN_OFFSET_Y == 30);
    assert(STACKCHAN_LEFT_EYE_X == 90 && STACKCHAN_LEFT_EYE_Y == 93);
    assert(STACKCHAN_RIGHT_EYE_X == 230 && STACKCHAN_RIGHT_EYE_Y == 96);
    assert(STACKCHAN_EYE_RADIUS == 8 && STACKCHAN_EYE_OPEN_STEPS == 12);
    assert(STACKCHAN_BACKGROUND_RGB == 0 && STACKCHAN_FOREGROUND_RGB == 0xFFFFFF);
    face_model_init(&model, false, 0);
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    assert_canonical_frame(&model, frame);
    assert_bounds(white_bounds(frame, 50, 80, 85, 115), 62, 94, 72, 105);
    assert_bounds(white_bounds(frame, 150, 80, 195, 115), 167, 96, 177, 107);
    for (unsigned open_step = 0; open_step <= 12; ++open_step) {
        model.eye_open_step = (uint8_t)open_step;
        model.blink_closed = open_step <= 2;
        assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
        assert_canonical_frame(&model, frame);
        if (open_step <= 2) assert(white_bounds(frame, 40, 80, 200, 115).count == 0);
    }
    for (unsigned state = 0; state < FACE_STATE_COUNT; ++state) {
        face_model_init(&model, false, 0);
        assert(event(&model, STATE_EVENTS[state], 0, 0));
        assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
        assert_canonical_frame(&model, frame);
        if (state != FACE_STATE_SLEEP) {
            model.eye_open_step = 6;
            assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
            assert_canonical_frame(&model, frame);
        }
    }
    const int breath_offsets[] = {-6, 0, 6};
    const double gazes[][2] = {{0, 0}, {0.2, -0.2}, {-0.2, 0.2}, {2.0, -2.0}};
    for (size_t breath = 0; breath < sizeof(breath_offsets) / sizeof(breath_offsets[0]); ++breath) {
        for (size_t gaze = 0; gaze < sizeof(gazes) / sizeof(gazes[0]); ++gaze) {
            face_model_init(&model, false, 0);
            model.breath_offset = (int8_t)breath_offsets[breath];
            model.gaze_x = gazes[gaze][0];
            model.gaze_y = gazes[gaze][1];
            assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
            assert_canonical_frame(&model, frame);
        }
    }
    free(frame);
}

static void test_canonical_mouth_levels(void)
{
    uint16_t *frame = malloc(FRAME_PIXELS * sizeof(*frame));
    assert(frame != NULL);
    const unsigned levels[] = {0, 50, 100};
    const int expected[][4] = {{86, 138, 153, 143}, {94, 129, 145, 153}, {101, 119, 138, 162}};
    assert(STACKCHAN_MOUTH_X == 160 && STACKCHAN_MOUTH_Y == 148);
    assert(STACKCHAN_MOUTH_MIN_WIDTH == 50 && STACKCHAN_MOUTH_MAX_WIDTH == 90);
    assert(STACKCHAN_MOUTH_MIN_HEIGHT == 8 && STACKCHAN_MOUTH_MAX_HEIGHT == 58);
    face_model_t model;
    face_model_init(&model, false, 0);
    assert(event(&model, FACE_EVENT_SPEAKING, 0, 0));
    for (size_t index = 0; index < sizeof(levels) / sizeof(levels[0]); ++index) {
        assert(event(&model, FACE_EVENT_MOUTH_LEVEL, (int)levels[index], 0));
        assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
        assert_canonical_frame(&model, frame);
        const bounds_t bounds = white_bounds(frame, 80, 114, 160, 175);
        assert_bounds(bounds, expected[index][0], expected[index][1], expected[index][2], expected[index][3]);
        assert(bounds.count == (size_t)(bounds.right - bounds.left + 1) *
               (size_t)(bounds.bottom - bounds.top + 1)); /* Solid rectangle, no hollow oval. */
    }
    free(frame);
}

static void test_face_roi_and_label_boundaries(void)
{
    uint16_t *frame = malloc(FRAME_PIXELS * sizeof(*frame));
    uint16_t *neutral = malloc(FRAME_PIXELS * sizeof(*neutral));
    assert(frame != NULL && neutral != NULL);
    face_model_t model;
    face_model_init(&model, false, 0);
    assert(face_render_rgb565(&model, neutral, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    const uint64_t idle_hash = frame_hash(neutral, FRAME_PIXELS);
    const face_event_kind_t neutral_events[] = {
        FACE_EVENT_IDLE, FACE_EVENT_LISTENING, FACE_EVENT_THINKING,
        FACE_EVENT_SPEAKING, FACE_EVENT_BLOCKED, FACE_EVENT_CONFUSED
    };
    for (size_t index = 0; index < sizeof(neutral_events) / sizeof(neutral_events[0]); ++index) {
        face_model_init(&model, false, 0);
        assert(event(&model, neutral_events[index], 0, 0));
        assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
        assert(memcmp(frame + 30 * FACE_WIDTH, neutral + 30 * FACE_WIDTH,
                      180 * FACE_WIDTH * sizeof(*frame)) == 0);
        assert(white_bounds(frame, 0, 210, 240, 240).count > 0);
        assert(white_bounds(frame, 0, 0, 240, 30).count == 0);
    }
    (void)face_model_tick(&model, 600);
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    assert(frame_hash(frame + 30 * FACE_WIDTH, 180 * FACE_WIDTH) !=
           frame_hash(neutral + 30 * FACE_WIDTH, 180 * FACE_WIDTH));
    assert_canonical_frame(&model, frame);
    face_model_init(&model, true, 0);
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    assert(frame_hash(frame, FRAME_PIXELS) != idle_hash);
    assert(white_bounds(frame, 0, 0, 240, 30).count > 100);
    assert(memcmp(frame + 30 * FACE_WIDTH, neutral + 30 * FACE_WIDTH,
                  180 * FACE_WIDTH * sizeof(*frame)) == 0);
    assert_canonical_frame(&model, frame);
    free(frame);
    free(neutral);
}

static FILE *open_fixture_file(const char *directory, const char *name)
{
    char path[4096];
    const int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fputs("fixture path is too long\n", stderr);
        return NULL;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) perror(path);
    return file;
}

static void write_bounds_json(FILE *file, bounds_t bounds)
{
    if (bounds.count == 0) {
        fputs("{\"pixelBBox\":null,\"whitePixels\":0}", file);
    } else {
        fprintf(file, "{\"pixelBBox\":[%d,%d,%d,%d],\"whitePixels\":%zu}",
                bounds.left, bounds.top, bounds.right, bounds.bottom, bounds.count);
    }
}

static bool dump_frame(const char *directory, const char *name,
                       const face_model_t *model, FILE *metadata, bool first)
{
    uint16_t *frame = malloc(FRAME_PIXELS * sizeof(*frame));
    uint8_t *rgb = malloc(FRAME_PIXELS * 3);
    assert(frame != NULL && rgb != NULL);
    assert(face_render_rgb565(model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    assert_canonical_frame(model, frame);
    for (size_t index = 0; index < FRAME_PIXELS; ++index) {
        rgb[index * 3] = (uint8_t)(((frame[index] >> 11) & 31) * 255 / 31);
        rgb[index * 3 + 1] = (uint8_t)(((frame[index] >> 5) & 63) * 255 / 63);
        rgb[index * 3 + 2] = (uint8_t)((frame[index] & 31) * 255 / 31);
    }
    char filename[80];
    const int length = snprintf(filename, sizeof(filename), "%s.ppm", name);
    assert(length >= 0 && (size_t)length < sizeof(filename));
    FILE *file = open_fixture_file(directory, filename);
    if (file == NULL) {
        free(frame);
        free(rgb);
        return false;
    }
    const bool header_written = fprintf(file, "P6\n%u %u\n255\n", (unsigned)FACE_WIDTH, (unsigned)FACE_HEIGHT) > 0;
    const bool image_written = fwrite(rgb, 3, FRAME_PIXELS, file) == FRAME_PIXELS;
    const bool closed = fclose(file) == 0;
    if (!first) fputs(",\n", metadata);
    fprintf(metadata, "{\"file\":\"%s\",\"state\":\"%s\",\"mouthLevel\":%u,\"eyeOpenStep\":%u,"
            "\"breath\":%d,\"gaze\":{\"x\":%.17g,\"y\":%.17g},\"faceHash\":\"%016" PRIx64 "\",\"leftEye\":",
            filename, face_state_name(model->state), (unsigned)model->mouth_level,
            (unsigned)model->eye_open_step, (int)model->breath_offset,
            model->gaze_x, model->gaze_y, frame_hash(frame + 30 * FACE_WIDTH, 180 * FACE_WIDTH));
    write_bounds_json(metadata, white_bounds(frame, 40, 75, 100, 115));
    fputs(",\"rightEye\":", metadata);
    write_bounds_json(metadata, white_bounds(frame, 140, 75, 200, 115));
    fputs(",\"mouth\":", metadata);
    write_bounds_json(metadata, white_bounds(frame, 75, 114, 165, 180));
    fputs("}", metadata);
    free(frame);
    free(rgb);
    return header_written && image_written && closed;
}

static void write_motion_json(FILE *file, uint32_t seed, uint64_t elapsed_ms)
{
    const face_motion_t motion = face_motion_sample(seed, elapsed_ms);
    const uint64_t quantized = elapsed_ms - elapsed_ms % STACKCHAN_MOTION_TICK_MS;
    fprintf(file, "{\"seed\":%" PRIu32 ",\"elapsedMs\":", seed);
    if (elapsed_ms > UINT64_C(9007199254740991)) fprintf(file, "\"%" PRIu64 "\"", elapsed_ms);
    else fprintf(file, "%" PRIu64, elapsed_ms);
    fputs(",\"timeMs\":", file);
    if (quantized > UINT64_C(9007199254740991)) fprintf(file, "\"%" PRIu64 "\"", quantized);
    else fprintf(file, "%" PRIu64, quantized);
    fprintf(file, ",\"eyeOpen\":%.17g,\"eyeOpenStep\":%u,\"breath\":%d,"
            "\"gaze\":{\"x\":%.17g,\"y\":%.17g},\"blinkSlot\":%u,\"gazeSlot\":%u,"
            "\"blinkOpenMs\":%u,\"blinkTransitionMs\":%u,\"blinkCycleMs\":%" PRIu64
            ",\"gazeCycleMs\":%" PRIu64 "}",
            motion.eye_open, (unsigned)motion.eye_open_step, (int)motion.breath_offset,
            motion.gaze_x, motion.gaze_y, motion.blink_slot, motion.gaze_slot,
            motion.blink_open_ms, motion.blink_transition_ms, motion.blink_cycle_ms,
            motion.gaze_cycle_ms);
}

static bool dump_fixtures(const char *directory)
{
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        perror(directory);
        return false;
    }
    FILE *geometry = open_fixture_file(directory, "geometry.json");
    if (geometry == NULL) return false;
    fputs("{\"sourceCommit\":\"2f6b5a65e30278fdbd1c5114cab6d42cdb7b7a0d\","
          "\"width\":240,\"height\":240,\"faceROI\":[0,30,240,180],"
          "\"pixelRule\":\"pixel_center\",\"projection\":{\"scale\":0.75,\"offsetX\":0,\"offsetY\":30},"
          "\"frames\":[\n", geometry);
    face_model_t model;
    bool ok = true;
    for (unsigned state = 0; state < FACE_STATE_COUNT && ok; ++state) {
        face_model_init(&model, false, 0);
        assert(event(&model, STATE_EVENTS[state], 0, 0));
        ok = dump_frame(directory, face_state_name(model.state), &model, geometry, state == 0);
    }
    if (ok) {
        face_model_init(&model, false, 0);
        model.eye_open_step = 2;
        model.blink_closed = true;
        ok = dump_frame(directory, "blink", &model, geometry, false);
    }
    const int levels[] = {0, 50, 100};
    const char *const names[] = {"speaking0", "speaking50", "speaking100"};
    for (size_t index = 0; index < sizeof(levels) / sizeof(levels[0]) && ok; ++index) {
        face_model_init(&model, false, 0);
        assert(event(&model, FACE_EVENT_SPEAKING, 0, 0));
        assert(event(&model, FACE_EVENT_MOUTH_LEVEL, levels[index], 0));
        ok = dump_frame(directory, names[index], &model, geometry, false);
    }
    fputs("],\"mouthGeometry\":["
          "{\"level\":0,\"native\":{\"x\":115,\"y\":144,\"width\":90,\"height\":8},"
          "\"projected\":{\"x\":86.25,\"y\":138,\"width\":67.5,\"height\":6}},"
          "{\"level\":50,\"native\":{\"x\":125,\"y\":132,\"width\":70,\"height\":33},"
          "\"projected\":{\"x\":93.75,\"y\":129,\"width\":52.5,\"height\":24.75}},"
          "{\"level\":100,\"native\":{\"x\":135,\"y\":119,\"width\":50,\"height\":58},"
          "\"projected\":{\"x\":101.25,\"y\":119.25,\"width\":37.5,\"height\":43.5}}]}\n", geometry);
    const bool geometry_closed = fclose(geometry) == 0;
    if (!ok || !geometry_closed) return false;
    FILE *motion_file = open_fixture_file(directory, "motion.json");
    if (motion_file == NULL) return false;
    fputs("{\"sampling\":\"33ms ticks; bounded repeating 32-slot Nemossi adaptation\",\"samples\":[\n", motion_file);
    const uint64_t times[] = {0, 33, 399, 400, 1000, 1500, 2000, 3000, 4500,
        6000, 12000, 16500, UINT64_C(0xFFFFFFFF), UINT64_MAX - 1, UINT64_MAX};
    const uint32_t seeds[] = {STACKCHAN_ANIMATION_SEED, 37, 48, UINT32_MAX};
    bool first = true;
    for (size_t seed = 0; seed < sizeof(seeds) / sizeof(seeds[0]); ++seed) {
        for (size_t time = 0; time < sizeof(times) / sizeof(times[0]); ++time) {
            if (!first) fputs(",\n", motion_file);
            write_motion_json(motion_file, seeds[seed], times[time]);
            first = false;
        }
        const face_motion_t initial = face_motion_sample(seeds[seed], 0);
        uint64_t start = 0;
        for (unsigned slot = 0; slot < STACKCHAN_ANIMATION_SLOTS; ++slot) {
            const uint64_t slot_tick = start + (33 - start % 33) % 33;
            const face_motion_t sample = face_motion_sample(seeds[seed], slot_tick);
            const uint64_t near_minimum = start + sample.blink_open_ms + sample.blink_transition_ms / 4;
            fputs(",\n", motion_file);
            write_motion_json(motion_file, seeds[seed], near_minimum);
            start += sample.blink_open_ms + sample.blink_transition_ms;
        }
        assert(start == initial.blink_cycle_ms);
    }
    fputs("]}\n", motion_file);
    return fclose(motion_file) == 0;
}

int main(int argc, char **argv)
{
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "--dump-ppm") != 0)) {
        fprintf(stderr, "usage: %s [--dump-ppm DIRECTORY]\n", argv[0]);
        return 2;
    }
    test_transitions_and_status();
    test_invalid_inputs_are_atomic();
    test_mouth_clamp_and_timeout();
    test_input_timeout_and_recovery();
    test_blink_and_clock_boundaries();
    test_motion_intervals_curve_and_large_time();
    test_motion_clock_is_independent_of_state();
    test_demo_is_explicit_and_deterministic();
    test_renderer_clips_and_rejects_short_buffers();
    test_canonical_raster_masks_and_motion();
    test_canonical_mouth_levels();
    test_face_roi_and_label_boundaries();
    puts("face model: 12 test groups passed (state, safety, canonical raster, motion, demo, RGB565)");
    if (argc == 3) {
        if (!dump_fixtures(argv[2])) return 1;
        printf("canonical PPM and geometry/motion fixtures written to %s\n", argv[2]);
    }
    return 0;
}
