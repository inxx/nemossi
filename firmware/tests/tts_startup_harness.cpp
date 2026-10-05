/* SPDX-License-Identifier: Apache-2.0
 * Actual patched startup/ready functions with synthetic allocation failures.
 * No tasks, sockets, audio, SDK installs, or device access.
 */
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#define ESP_LOGE(...) ((void)0)
#define pdPASS 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define EV_TEXT 72
#define MIC_RATE 16000
#define IN_BYTES 256000
#define OUT_BYTES 64000
#define DICT_CHUNK_BYTES 8192
#define VOICE_NOTE 1
#define NOTE_PART_BYTES 6144
#define MP3_BUF 524288
#define MAX_MSGS 8
#define TEXT_MAX 1024
#define MINIMP3_MAX_SAMPLES_PER_FRAME 2304
#define NDJSON_LINE_MAX 16384
#define MUSE_HATCH_NOTE_TAIL "synthetic"
struct cmd_t { unsigned fields[4]; };
struct ev_t { unsigned fields[4]; };
struct cJSON_Hooks { void *(*allocate)(size_t); void (*release)(void *); };
using QueueHandle_t = void *;
using SemaphoreHandle_t = void *;
static QueueHandle_t s_cmds, s_events, s_in, s_out;
static SemaphoreHandle_t s_local_commit_lock;
static std::atomic<bool> s_local_session_started{false};
struct turn_t { uint8_t *chunk, *mp3, *note; char *texts; int tts_msg; };
static turn_t s_turn;
static int16_t *s_pcm, *s_pcm16;
struct stream_t { char *line; size_t cap; };
static stream_t s_streams[6];
static unsigned failure, queue_calls, stream_calls, alloc_calls, created, assertions;
#define CHECK(x) do { ++assertions; assert(x); } while (0)
static void *json_alloc(size_t) { return nullptr; }
static void heap_caps_free(void *) {}
static void cJSON_InitHooks(const cJSON_Hooks *) {}
static bool nm_tts_init() { return false; } /* Disabled TTS still starts an error-capable session. */
static bool muse_hatch_configured() { return true; }
static bool muse_wifi_connected() { return true; }
static void hatch_task(void *) {}
static SemaphoreHandle_t xSemaphoreCreateMutex() { return failure == 1 ? nullptr : (void *)1; }
static QueueHandle_t xQueueCreateWithCaps(unsigned, size_t, unsigned) {
    ++queue_calls;
    if ((failure == 2 && queue_calls == 1) || (failure == 3 && queue_calls == 2)) return nullptr;
    return (void *)1;
}
static QueueHandle_t xQueueCreate(unsigned, size_t) { return nullptr; }
static QueueHandle_t xStreamBufferCreateWithCaps(size_t, unsigned, unsigned) {
    ++stream_calls;
    if (failure == stream_calls + 3) return nullptr;
    return (void *)1;
}
static void *psram_alloc(size_t) {
    ++alloc_calls;
    /* Required chunk/mp3/note and PCM allocations (caption text is optional upstream). */
    if ((failure >= 6 && failure <= 8 && alloc_calls == failure - 5) ||
        (failure >= 9 && failure <= 10 && alloc_calls == failure - 4) ||
        (failure >= 12 && failure <= 17 && alloc_calls == failure - 5)) return nullptr;
    return (void *)1;
}
static int xTaskCreatePinnedToCoreWithCaps(void (*task)(void *), const char *, unsigned, void *,
                                         unsigned, void *, unsigned, unsigned) {
    assert(task == hatch_task); ++created; return failure == 11 ? 0 : pdPASS;
}

/* NM_EXTRACTED_STARTUP_FUNCTIONS */

int main() {
    for (unsigned f = 0; f <= 17; ++f) {
        failure = f; queue_calls = stream_calls = alloc_calls = created = 0;
        s_cmds = s_events = s_in = s_out = nullptr;
        s_local_commit_lock = nullptr; s_local_session_started.store(false);
        CHECK(!muse_hatch_ready());
        muse_hatch_start();
        CHECK(muse_hatch_ready() == (f == 0));
        CHECK(s_local_session_started.load() == (f == 0));
        CHECK(created == ((f == 0 || f == 11) ? 1u : 0u));
    }
    printf("local TTS session startup: %u assertions passed\n", assertions);
}
