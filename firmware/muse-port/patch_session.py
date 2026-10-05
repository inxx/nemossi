#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) Meta Platforms, Inc. and affiliates. (upstream portions)
# Nemossi modifications: bounded local TTS, cancellation and OOM guards.
"""Patch a fresh staging copy of the pinned Apache-2.0 Muse chat session.

Upstream: Meta Platforms, muse-gadget-sdk
commit 3229892e93c18a768ace42cbe1fe7133f91ca203.
The SDK checkout remains read-only. A second application is deliberately rejected.
"""
import hashlib
from pathlib import Path

SOURCE_SHA256 = "e939a1424ca04668fc5b101d39a2a53a03538e5a544fdf9ad2feb30286f3811c"

START_TTS = r'''static void start_tts(void)
{
    if (s_turn.tts_msg >= 0 || s_turn.gen != s_gen.load()) return;
    for (int i = 0; i < s_turn.nmsgs; ++i) {
        msg_t &m = s_turn.msgs[i];
        if (m.tts != TTS_QUEUED) continue;
        if (!nm_tts_enabled()) {
            turn_fail("LOCAL TTS NOT CONFIGURED");
            return;
        }
        if (!s_turn.texts || m.len >= TEXT_MAX ||
            strlen(s_turn.texts + i * TEXT_MAX) != m.len) {
            turn_fail("LOCAL TTS TEXT LIMIT");
            return;
        }
        s_local_tts_id = nm_tts_start(s_turn.texts + i * TEXT_MAX);
        if (!s_local_tts_id) {
            turn_fail("LOCAL TTS FAILED");
            return;
        }
        m.pcm_start = s_turn.pcm_out;
        m.pcm_frames = 0;
        m.tts = TTS_ACTIVE;
        s_turn.tts_msg = i;
        s_turn.silent = false;
        s_local_count = s_local_offset = 0;
        mark(M_TTS);
        show_reply_start(m);
        return;
    }
}

/* Session task is the sole s_out writer. HTTP runs on the local TTS worker;
 * retain partial sends under backpressure and count only accepted samples.
 */
static void drain_local_tts(void)
{
    if (s_turn.tts_msg < 0 || !s_local_tts_id || s_turn.gen != s_gen.load()) return;
    size_t room = xStreamBufferSpacesAvailable(s_out) / sizeof(int16_t);
    if (!room) return;
    if (s_local_offset == s_local_count) {
        size_t count = 0;
        uint32_t total = 0;
        size_t cap = room < 256 ? room : 256;
        nm_tts_status_t state = nm_tts_read(s_local_tts_id, s_local_pcm, cap, &count, &total);
        if (s_turn.gen != s_gen.load()) return;
        if (state == NM_TTS_WAIT) return;
        if (state == NM_TTS_FAILED || state == NM_TTS_STALE) {
            turn_fail("LOCAL TTS FAILED");
            return;
        }
        msg_t &m = s_turn.msgs[s_turn.tts_msg];
        if (state == NM_TTS_DONE) {
            m.pcm_frames = s_turn.pcm_out - m.pcm_start;
            m.tts = TTS_FINISHED;
            s_turn.tts_msg = -1;
            s_local_tts_id = 0;
            return;
        }
        m.pcm_frames = total;
        s_local_count = count;
        s_local_offset = 0;
    }
    size_t pending = s_local_count - s_local_offset;
    if (pending > room) pending = room;
    if (xSemaphoreTake(s_local_commit_lock, 0) != pdTRUE) return;
    if (s_turn.gen != s_gen.load()) {
        xSemaphoreGive(s_local_commit_lock);
        return;
    }
    size_t sent = xStreamBufferSend(s_out, s_local_pcm + s_local_offset,
                                  pending * sizeof(int16_t), 0) / sizeof(int16_t);
    xSemaphoreGive(s_local_commit_lock);
    s_local_offset += sent;
    s_turn.pcm_out += sent;
    if (sent) mark(M_AUDIO);
}

'''


def patched_source(source: bytes) -> bytes:
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise ValueError("Muse session source does not match the pinned SHA-256")
    text = source.decode("utf-8")

    def replace(old: str, new: str) -> None:
        nonlocal text
        if text.count(old) != 1:
            raise ValueError("Muse patch anchor is not unique")
        text = text.replace(old, new, 1)

    replace('#include "cJSON.h"', '#include "cJSON.h"\n#include "tts_client.h"')
    replace('#include "freertos/queue.h"', '#include "freertos/queue.h"\n#include "freertos/semphr.h"')
    replace('static std::atomic<bool> s_resting{false};', '''static std::atomic<bool> s_resting{false};
/* Nemossi modification: optional local Mac PCM TTS, bounded and cancellable. */
static uint32_t s_local_tts_id;
static SemaphoreHandle_t s_local_commit_lock;
static std::atomic<bool> s_local_session_started{false};
static int16_t s_local_pcm[256];
static size_t s_local_count, s_local_offset;''')
    replace('static void turn_reset_streams(void)\n{', '''static void turn_reset_streams(void)
{
    nm_tts_cancel();
    s_local_tts_id = 0;
    s_local_count = s_local_offset = 0;''')
    replace('    resampler_init(&s_turn.up, MIC_RATE, DICT_RATE);', '''    resampler_init(&s_turn.up, MIC_RATE, DICT_RATE);
    if (!text && !nm_tts_enabled()) {
        turn_fail("LOCAL TTS NOT CONFIGURED");
        return false;
    }''')
    start = text.index('static void start_tts(void)')
    end = text.index('static void tts_data(', start)
    text = text[:start] + START_TTS + text[end:]
    # The upstream MP3 decoder remains for its self-test and Noise stream
    # compatibility, but voice replies enter the PCM path above instead.
    replace('            decode();', '''            if (s_local_tts_id) drain_local_tts();
            else decode();''')
    replace('    cJSON_InitHooks(&hooks);', '''    cJSON_InitHooks(&hooks);
    s_local_commit_lock = xSemaphoreCreateMutex();
    nm_tts_init();''')
    replace('''    for (auto &s : s_streams) {
        s.line = static_cast<char *>(psram_alloc(NDJSON_LINE_MAX));
        s.cap = NDJSON_LINE_MAX;
    }''', '''    bool local_stream_lines_ready = true;
    for (auto &s : s_streams) {
        s.line = static_cast<char *>(psram_alloc(NDJSON_LINE_MAX));
        s.cap = NDJSON_LINE_MAX;
        if (!s.line) local_stream_lines_ready = false;
    }''')
    replace('    if (!s_cmds || !s_events || !s_in || !s_out || !s_turn.chunk',
            '    if (!local_stream_lines_ready || !s_local_commit_lock || !s_cmds || !s_events || !s_in || !s_out || !s_turn.chunk')
    replace('''        ESP_LOGE(TAG, "start failed");
    }
}

extern "C" void muse_hatch_chat_connect''', '''        ESP_LOGE(TAG, "start failed");
    } else {
        s_local_session_started.store(true);
    }
}

extern "C" void muse_hatch_chat_connect''')
    replace('    return s_cmds && muse_hatch_configured() && muse_wifi_connected();',
            '    return s_local_session_started.load() && s_cmds && muse_hatch_configured() && muse_wifi_connected();')
    replace('    uint32_t gen = ++s_gen;', '''    /* No network or audio I/O under this lock: serialize invalidation with
     * the session's bounded PCM enqueue, then drain after old commits finish.
     */
    if (s_local_commit_lock) xSemaphoreTake(s_local_commit_lock, portMAX_DELAY);
    uint32_t gen = ++s_gen;
    nm_tts_cancel();
    if (s_local_commit_lock) xSemaphoreGive(s_local_commit_lock);''')
    replace('''    uint32_t gen = s_gen.load();
    ++s_gen;
    post(CMD_CANCEL, gen);''', '''    if (s_local_commit_lock) xSemaphoreTake(s_local_commit_lock, portMAX_DELAY);
    uint32_t gen = s_gen.load();
    ++s_gen;
    nm_tts_cancel();
    if (s_local_commit_lock) xSemaphoreGive(s_local_commit_lock);
    post(CMD_CANCEL, gen);''')
    # Even if the command queue is full, generation invalidation finishes an
    # old voice turn when the session task next runs, without waiting for I/O.
    replace('        if (!s_connected) {\n            if (!muse_wifi_connected()) {', '''        if (s_turn.phase != P_IDLE && !s_turn.text && s_turn.gen != s_gen.load()) {
            turn_finish();
        }
        if (!s_connected) {
            if (!muse_wifi_connected()) {''')
    replace('''    return xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);''', '''    uint32_t gen = s_gen.load();
    size_t n = xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
    if (gen != s_gen.load()) {
        memset(pcm, 0, n * sizeof(int16_t));
        return 0;
    }
    return n;''')
    return text.encode("utf-8")


def apply_session_patch(path: Path) -> None:
    """Apply once to a fresh copy; validates before any write."""
    path = Path(path)
    path.write_bytes(patched_source(path.read_bytes()))
