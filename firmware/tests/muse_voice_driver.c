/* SPDX-License-Identifier: Apache-2.0
 * Runs the exact selected voice.c bodies against adversarial player timing.
 */
static unsigned cancel_calls, mic_start_calls, begin_calls, wait_calls, reply_success_logs;
static unsigned failures, idle_calls;
static bool player_failed, idle_ready = true, wait_times_out;
static bool fail_on_end, reply_pcm_once;
static unsigned writes;
static unsigned stop_calls, operation_order, stop_order, wait_order, mic_order;
static led_voice_t last_state = LED_VOICE_IDLE;

void host_log(bool success, const char *format, ...) {
    if (success && !strncmp(format, "reply:", 6)) ++reply_success_logs;
    if (!success && !strncmp(format, "turn failed:", 12)) ++failures;
}
int xQueueReceive(QueueHandle_t queue, void *event, TickType_t timeout) {
    (void)queue; (void)event; (void)timeout; return pdFALSE;
}
int64_t esp_timer_get_time(void) { return 1000000; }
void led_status_set_voice(led_voice_t state) { last_state = state; if (state == LED_VOICE_IDLE) ++idle_calls; }
void led_status_set_level(float level) { (void)level; }
void voice_player_begin(void) { player_failed = false; }
void voice_player_stop(void) { ++stop_calls; stop_order = ++operation_order; }
esp_err_t voice_player_write(const int16_t *pcm, size_t frames) {
    assert(pcm && frames == 2 && pcm[0] == INT16_MIN && pcm[1] == INT16_MAX);
    ++writes; return ESP_OK;
}
void voice_player_end(void) {
    /* Schedule the final I2S failure and its tail before the caller's first
     * wait; the inner reply loop previously saw failed=false. */
    if (fail_on_end) player_failed = true;
    idle_ready = true;
}
bool voice_player_started(void) { return false; }
bool voice_player_failed(void) { return player_failed; }
bool voice_player_wait(int timeout_ms) {
    ++wait_calls;
    if (timeout_ms) {
        assert(timeout_ms == 500 && stop_calls && stop_order);
        wait_order = ++operation_order;
        if (wait_times_out) return false;
        idle_ready = true;
    }
    return idle_ready;
}
esp_err_t voice_board_mic_start(void) {
    ++mic_start_calls;
    mic_order = ++operation_order;
    /* The old player was still busy when run_turn() was called. */
    assert(idle_ready && wait_calls > 0 && stop_order < wait_order && wait_order < mic_order);
    return ESP_OK;
}
void voice_board_mic_stop(void) {}
size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    (void)pcm; (void)frames; if (peak) *peak = 0; return 0;
}
void muse_hatch_turn_cancel(void) { ++cancel_calls; }
void muse_hatch_turn_begin(void) { ++begin_calls; }
void muse_hatch_turn_end(void) {}
void muse_hatch_turn_audio(const int16_t *pcm, size_t frames) { (void)pcm; (void)frames; }
muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap) {
    static bool emitted;
    assert(cap > 0); text[0] = '\0';
    if (!emitted) { emitted = true; return MUSE_HATCH_EV_DONE; }
    return MUSE_HATCH_EV_NONE;
}
size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms) {
    (void)wait_ms;
    if (reply_pcm_once) {
        assert(frames >= 2); reply_pcm_once = false;
        pcm[0] = INT16_MIN; pcm[1] = INT16_MAX; return 2;
    }
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "final_failure")) {
        /* I2S failed, the worker tail ended, and IDLE was set before this
         * caller was scheduled. The while(wait) body therefore never runs. */
        fail_on_end = reply_pcm_once = true;
        assert(!reply());
        assert(writes == 1 && wait_calls == 1 && cancel_calls == 1 && failures == 1);
        assert(last_state == LED_VOICE_ERROR && !reply_success_logs && !idle_calls);
    } else if (!strcmp(argv[1], "reply_ok")) {
        reply_pcm_once = true;
        assert(!reply());
        assert(writes == 1 && wait_calls == 1 && !cancel_calls && !failures);
        assert(last_state == LED_VOICE_IDLE && reply_success_logs == 1);
    } else if (!strcmp(argv[1], "capture_handoff")) {
        idle_ready = false;
        assert(!run_turn());
        assert(wait_calls == 1 && mic_start_calls == 1 && begin_calls == 1);
        assert(stop_calls == 1 && stop_order < wait_order && wait_order < mic_order);
        assert(last_state == LED_VOICE_ERROR && failures == 1); /* no mic fixture */
    } else {
        assert(!strcmp(argv[1], "capture_timeout"));
        idle_ready = false; wait_times_out = true;
        assert(!run_turn());
        assert(wait_calls == 1 && !mic_start_calls && !begin_calls);
        assert(stop_calls == 1 && stop_order < wait_order);
        assert(last_state == LED_VOICE_ERROR && failures == 1);
    }
    puts("actual staged Muse voice boundary passed");
    return 0;
}
