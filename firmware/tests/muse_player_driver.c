/* SPDX-License-Identifier: Apache-2.0
 * Appended after the actual staged player C, so its real private state and
 * worker execute. The scheduler terminates after one completed worker cycle.
 */
static uint8_t stream[400000];
static size_t stream_bytes;
static unsigned events, alloc_calls, fail_allocation, task_creates, speaker_calls;
static unsigned amp_on_calls, amp_off_calls, notify_calls, error_states;
static bool speaker_fails;
static void (*created_worker)(void *);
static jmp_buf worker_boundary;

void host_log(bool success, const char *format, ...) { (void)success; (void)format; }
void *heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(caps == MALLOC_CAP_SPIRAM);
    return ++alloc_calls == fail_allocation ? NULL : malloc(bytes);
}
StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t bytes, size_t trigger, unsigned caps) {
    assert(bytes < sizeof(stream) && trigger == 1 && caps == MALLOC_CAP_SPIRAM);
    return stream;
}
EventGroupHandle_t xEventGroupCreate(void) { return &events; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    assert(group == &events); events |= bits; return events;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    assert(group == &events); events &= ~bits; return events;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                              int clear, int all, TickType_t timeout) {
    assert(group == &events && !clear && all && bits == BIT0);
    (void)timeout; return events;
}
int xTaskCreateWithCaps(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *handle, unsigned caps) {
    assert(!strcmp(name, "voice_play") && stack >= 3072 && !arg && priority == 5);
    assert(caps == MALLOC_CAP_SPIRAM && s_input_pcm && s_output_pcm);
    ++task_creates; created_worker = task; *handle = &events; return pdPASS;
}
unsigned ulTaskNotifyTake(int clear, TickType_t timeout) {
    assert(clear && timeout == portMAX_DELAY);
    if (++notify_calls > 1) longjmp(worker_boundary, 1);
    return 1;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task == &events); }
void vTaskDelay(TickType_t ticks) { assert(ticks <= 120); }
int vTaskSuspend(void *task) { (void)task; assert(!"A failed init must never suspend a worker"); return 0; }
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t buffer) { assert(buffer == stream); return stream_bytes; }
size_t xStreamBufferReceive(StreamBufferHandle_t buffer, void *out, size_t bytes, TickType_t timeout) {
    assert(buffer == stream && timeout <= 20);
    if (bytes > stream_bytes) bytes = stream_bytes;
    memcpy(out, stream, bytes); memmove(stream, stream + bytes, stream_bytes - bytes);
    stream_bytes -= bytes; return bytes;
}
size_t xStreamBufferSend(StreamBufferHandle_t buffer, const void *in, size_t bytes, TickType_t timeout) {
    assert(buffer == stream && timeout <= 100 && stream_bytes + bytes <= sizeof(stream));
    memcpy(stream + stream_bytes, in, bytes); stream_bytes += bytes; return bytes;
}
int xStreamBufferIsEmpty(StreamBufferHandle_t buffer) { assert(buffer == stream); return stream_bytes == 0; }
int xStreamBufferReset(StreamBufferHandle_t buffer) { assert(buffer == stream); stream_bytes = 0; return pdPASS; }
esp_err_t voice_board_speaker_write(const int32_t *pcm, size_t frames) {
    assert(pcm && frames == 6);
    /* INT16_MIN/MAX are converted by the real upsample body under UBSan. */
    assert(pcm[4] == INT32_MIN && pcm[5] == INT32_MIN);
    assert(pcm[10] == 32767 * 65536 && pcm[11] == 32767 * 65536);
    ++speaker_calls; return speaker_fails ? ESP_FAIL : ESP_OK;
}
void voice_board_amp(bool on) { if (on) ++amp_on_calls; else ++amp_off_calls; }
void led_status_set_voice(led_voice_t state) { assert(state == LED_VOICE_ERROR); ++error_states; }

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!strcmp(TAG, "link.voice_player"));
    if (!strcmp(argv[1], "oom1") || !strcmp(argv[1], "oom2")) {
        fail_allocation = (unsigned)(argv[1][3] - '0');
        assert(voice_player_init() == ESP_ERR_NO_MEM);
        assert(task_creates == 0 && !s_input_pcm && !s_output_pcm);
    } else {
        speaker_fails = !strcmp(argv[1], "speaker_fail");
        assert(speaker_fails || !strcmp(argv[1], "speaker_ok"));
        assert(voice_player_init() == ESP_OK && task_creates == 1 && created_worker);
        assert(voice_player_wait(0));
        voice_player_begin();
        const int16_t pcm[] = {INT16_MIN, INT16_MAX};
        assert(voice_player_write(pcm, 2) == ESP_OK);
        voice_player_end();
        if (!setjmp(worker_boundary)) created_worker(NULL);
        assert(speaker_calls == 1 && amp_on_calls == 1 && amp_off_calls == 1);
        assert(voice_player_wait(0));
        assert(voice_player_failed() == speaker_fails);
        assert(voice_player_started() == !speaker_fails && error_states == (unsigned)speaker_fails);
        free(s_input_pcm); free(s_output_pcm);
    }
    puts("actual staged Muse player scenario passed");
    return 0;
}
