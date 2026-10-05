/* SPDX-License-Identifier: Apache-2.0
 * Compiled with actual functions extracted from a freshly hash-checked patch.
 * Synthetic PCM/RTOS sinks only: no network, board, or credential access.
 */
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "tts_client.h"

#define TEXT_MAX 1024
#define MIC_RATE 16000
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
using SemaphoreHandle_t = std::mutex *;
static std::mutex commit_mutex;
static SemaphoreHandle_t s_local_commit_lock = &commit_mutex;
static std::atomic<uint32_t> s_gen{1};
static uint32_t s_local_tts_id;
static int16_t s_local_pcm[256];
static size_t s_local_count, s_local_offset;
enum tts_t { TTS_NONE, TTS_QUEUED, TTS_ACTIVE, TTS_FINISHED };
struct msg_t { size_t len; tts_t tts; uint32_t pcm_start, pcm_frames; };
struct turn_t { int tts_msg; uint32_t gen; int nmsgs; char *texts; msg_t msgs[2]; uint32_t pcm_out; bool silent; };
static turn_t s_turn;
static char texts[2048];
static int s_out = 1, s_in = 2;
static unsigned assertions;
#define CHECK(x) do { ++assertions; assert(x); } while (0)
static std::vector<int16_t> out, fixture;
static size_t fixture_offset;
static unsigned fixture_id, cancel_calls, read_calls;
static bool enabled = true;
static bool cancel_during_read;
static nm_tts_status_t fixture_mode = NM_TTS_PCM;
static size_t output_capacity = 19, send_limit = 3;
static char failure[72];
static std::mutex barrier_mutex;
static std::condition_variable barrier;
static bool pause_send, send_entered, release_send, invalidation_waiting;
static bool waiting_seen, enqueue_seen;

static int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t wait) {
    if (!wait) return mutex->try_lock();
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        invalidation_waiting = true; barrier.notify_all();
    }
    mutex->lock(); return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
static size_t xStreamBufferSpacesAvailable(int) { return (output_capacity - out.size()) * 2; }
static size_t xStreamBufferSend(int, const void *pcm, size_t bytes, int wait) {
    assert(!wait);
    if (pause_send) {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        send_entered = true; barrier.notify_all();
        barrier.wait(lock, [] { return release_send; });
    }
    size_t n = std::min({bytes / 2, output_capacity - out.size(), send_limit});
    const int16_t *data = static_cast<const int16_t *>(pcm);
    out.insert(out.end(), data, data + n); return n * 2;
}
static size_t xStreamBufferReceive(int, void *pcm, size_t bytes, int) {
    size_t n = std::min(bytes / 2, out.size());
    if (n) memcpy(pcm, out.data(), n * 2);
    out.erase(out.begin(), out.begin() + n);
    if (cancel_during_read) { cancel_during_read = false; ++s_gen; }
    return n * 2;
}
static void xStreamBufferReset(int) {}
static void drain_out() { out.clear(); }
enum cmd_type_t { CMD_BEGIN, CMD_CANCEL };
static void post(cmd_type_t, uint32_t) {}
enum mark_t { M_TTS, M_AUDIO };
static void mark(mark_t) {}
static void show_reply_start(const msg_t &) {}
static void turn_fail(const char *why) { strcpy(failure, why); s_turn.tts_msg = -1; nm_tts_cancel(); }

extern "C" bool nm_tts_enabled() { return enabled; }
extern "C" uint32_t nm_tts_start(const char *text) {
    assert(!strcmp(text, "fixture")); fixture_offset = 0; return ++fixture_id;
}
extern "C" void nm_tts_cancel() { ++cancel_calls; ++fixture_id; }
extern "C" nm_tts_status_t nm_tts_read(uint32_t id, int16_t *pcm, size_t cap, size_t *frames, uint32_t *total) {
    ++read_calls; *frames = 0; *total = 0;
    if (id != fixture_id) return NM_TTS_STALE;
    if (fixture_mode != NM_TTS_PCM) return fixture_mode;
    *total = static_cast<uint32_t>(fixture.size());
    if (fixture_offset == fixture.size()) return NM_TTS_DONE;
    *frames = std::min(cap, fixture.size() - fixture_offset);
    std::copy_n(fixture.begin() + fixture_offset, *frames, pcm); fixture_offset += *frames; return NM_TTS_PCM;
}

/* NM_EXTRACTED_SESSION_FUNCTIONS */

static void setup(size_t n = 513) {
    s_turn = turn_t{}; s_turn.tts_msg = -1; s_turn.gen = s_gen.load(); s_turn.nmsgs = 1;
    strcpy(texts, "fixture"); s_turn.texts = texts; s_turn.msgs[0].len = 7; s_turn.msgs[0].tts = TTS_QUEUED;
    s_local_tts_id = 0; s_local_count = s_local_offset = 0;
    fixture.clear(); for (size_t i = 0; i < n; ++i) fixture.push_back(static_cast<int16_t>(i - 256));
    out.clear(); failure[0] = '\0'; enabled = true; fixture_mode = NM_TTS_PCM;
    fixture_offset = 0; read_calls = 0; output_capacity = 19; send_limit = 3;
    pause_send = send_entered = release_send = invalidation_waiting = false;
    cancel_during_read = false;
}
static void backpressure() {
    setup(); start_tts(); CHECK(s_turn.tts_msg == 0 && !s_turn.silent && s_local_tts_id);
    std::vector<int16_t> played;
    for (unsigned guard = 0; guard < 2000 && s_turn.tts_msg >= 0; ++guard) {
        drain_local_tts();
        if (out.size() == output_capacity) {
            unsigned before = read_calls; size_t count = s_local_count, offset = s_local_offset;
            drain_local_tts(); CHECK(read_calls == before && s_local_count == count && s_local_offset == offset);
            played.insert(played.end(), out.begin(), out.end()); out.clear();
        }
    }
    played.insert(played.end(), out.begin(), out.end());
    CHECK(played == fixture); CHECK(s_turn.msgs[0].tts == TTS_FINISHED);
    CHECK(s_turn.pcm_out == 513 && s_turn.msgs[0].pcm_frames == 513 && s_local_tts_id == 0);
}
static void failures() {
    setup(); enabled = false; start_tts(); CHECK(!strcmp(failure, "LOCAL TTS NOT CONFIGURED"));
    setup(); s_turn.msgs[0].len = 1024; start_tts(); CHECK(!strcmp(failure, "LOCAL TTS TEXT LIMIT"));
    setup(); s_turn.texts = nullptr; start_tts(); CHECK(!strcmp(failure, "LOCAL TTS TEXT LIMIT"));
    setup(); start_tts(); fixture_mode = NM_TTS_FAILED; drain_local_tts(); CHECK(!strcmp(failure, "LOCAL TTS FAILED") && out.empty());
    setup(); start_tts(); fixture_mode = NM_TTS_WAIT; drain_local_tts(); CHECK(out.empty() && s_turn.tts_msg == 0);
    fixture_mode = NM_TTS_PCM; commit_mutex.lock(); drain_local_tts();
    CHECK(out.empty() && s_local_count > 0 && s_local_offset == 0); commit_mutex.unlock();
    ++s_gen; drain_local_tts(); CHECK(out.empty());
}
static void enqueue_invalidation_race(bool begin) {
    setup(12); start_tts(); pause_send = true;
    uint32_t old = s_gen.load();
    std::thread sender([] { drain_local_tts(); });
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier.wait(lock, [] { return send_entered; });
        enqueue_seen = send_entered;
    }
    std::thread invalidate([begin] { if (begin) muse_hatch_turn_begin(); else muse_hatch_turn_cancel(); });
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier.wait(lock, [] { return invalidation_waiting; });
        waiting_seen = invalidation_waiting;
        CHECK(s_gen.load() == old); /* Invalidator waits for the committed enqueue. */
        release_send = true; barrier.notify_all();
    }
    sender.join(); invalidate.join(); pause_send = false;
    CHECK(enqueue_seen && waiting_seen && s_gen.load() == old + 1 && out.empty());
    drain_local_tts(); CHECK(out.empty()); /* Old generation can never refill after drain. */
}
static void read_invalidation() {
    setup(); out = {123, -456}; int16_t pcm[4] = {1, 1, 1, 1};
    cancel_during_read = true;
    CHECK(muse_hatch_turn_read(pcm, 4, 20) == 0 && pcm[0] == 0 && pcm[1] == 0);
    out = {123, -456};
    CHECK(muse_hatch_turn_read(pcm, 4, 0) == 2 && pcm[0] == 123 && pcm[1] == -456);
}
int main() {
    backpressure(); failures(); enqueue_invalidation_race(false); enqueue_invalidation_race(true); read_invalidation();
    printf("local TTS session: %u assertions passed\n", assertions);
}
