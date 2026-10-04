#include "face.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    face_model_t other_seed;
    face_model_init_seeded(&model, false, 500, 37);
    face_model_init_seeded(&other_seed, false, 500, 48);
    assert(!model.blink_closed);
    unsigned first_offset = 0;
    bool offsets_vary = false;
    bool seeds_differ = false;
    for (unsigned slot = 0; slot < 6; ++slot) {
        unsigned closed_ms = 0;
        unsigned onset = 0;
        for (unsigned phase = 0; phase < FACE_BLINK_PERIOD_MS; ++phase) {
            const uint64_t now_ms = 500 + slot * FACE_BLINK_PERIOD_MS + phase;
            (void)face_model_tick(&model, now_ms);
            (void)face_model_tick(&other_seed, now_ms);
            if (model.blink_closed != other_seed.blink_closed) seeds_differ = true;
            if (model.blink_closed) {
                if (closed_ms == 0) onset = phase;
                ++closed_ms;
            }
        }
        assert(closed_ms == FACE_BLINK_DURATION_MS);
        assert(onset >= FACE_BLINK_EARLIEST_MS);
        assert(onset <= FACE_BLINK_PERIOD_MS - FACE_BLINK_DURATION_MS);
        if (slot == 0) first_offset = onset;
        if (onset != first_offset) offsets_vary = true;
    }
    assert(offsets_vary && seeds_differ);
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
}

static void test_frames_change_with_state_animation_and_levels(void)
{
    uint16_t *frame = malloc(FRAME_PIXELS * sizeof(*frame));
    assert(frame != NULL);
    face_model_t model;
    face_model_init(&model, false, 0);
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    const uint64_t idle_hash = frame_hash(frame, FRAME_PIXELS);
    (void)face_model_tick(&model, 600);
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    assert(frame_hash(frame, FRAME_PIXELS) != idle_hash);
    for (uint64_t now_ms = FACE_BLINK_EARLIEST_MS;
         now_ms < FACE_BLINK_PERIOD_MS && !model.blink_closed; ++now_ms) {
        (void)face_model_tick(&model, now_ms);
    }
    assert(model.blink_closed);
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    assert(frame_hash(frame, FRAME_PIXELS) != idle_hash);
    uint64_t hashes[FACE_STATE_COUNT];
    for (unsigned state = 0; state < FACE_STATE_COUNT; ++state) {
        face_model_init(&model, false, 0);
        assert(event(&model, STATE_EVENTS[state], 0, 1));
        assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
        hashes[state] = frame_hash(frame, FRAME_PIXELS);
        for (unsigned previous = 0; previous < state; ++previous) {
            assert(hashes[previous] != hashes[state]);
        }
    }
    face_model_init(&model, false, 0);
    assert(event(&model, FACE_EVENT_THINKING, 0, 1));
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    const uint64_t thinking_hash = frame_hash(frame, FRAME_PIXELS);
    (void)face_model_tick(&model, 451);
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    assert(frame_hash(frame, FRAME_PIXELS) != thinking_hash);
    assert(event(&model, FACE_EVENT_SPEAKING, 0, 452));
    assert(event(&model, FACE_EVENT_MOUTH_LEVEL, 0, 453));
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    const uint64_t closed_hash = frame_hash(frame, FRAME_PIXELS);
    assert(event(&model, FACE_EVENT_MOUTH_LEVEL, 100, 454));
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    assert(frame_hash(frame, FRAME_PIXELS) != closed_hash);
    face_model_init(&model, true, 0);
    assert(face_render_rgb565(&model, frame, FRAME_PIXELS, FACE_WIDTH, FACE_HEIGHT));
    assert(frame_hash(frame, FRAME_PIXELS) != idle_hash);
    /* The DEMO text is visible in the top area, above all face primitives. */
    size_t demo_pixels = 0;
    for (size_t y = 17; y < 31; ++y) {
        for (size_t x = 80; x < 160; ++x) {
            if (frame[y * FACE_WIDTH + x] != frame[0]) ++demo_pixels;
        }
    }
    assert(demo_pixels > 100);
    free(frame);
}

int main(void)
{
    test_transitions_and_status();
    test_invalid_inputs_are_atomic();
    test_mouth_clamp_and_timeout();
    test_input_timeout_and_recovery();
    test_blink_and_clock_boundaries();
    test_demo_is_explicit_and_deterministic();
    test_renderer_clips_and_rejects_short_buffers();
    test_frames_change_with_state_animation_and_levels();
    puts("face model: 8 test groups passed (state, safety, timing, demo, RGB565)");
    return 0;
}
