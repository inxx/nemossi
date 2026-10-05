/* SPDX-License-Identifier: Apache-2.0
 * Executes production TTS code with synthetic socket/RTOS/allocator fakes.
 * No socket is opened, no server contacted, and no audio device used.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../muse-port/tts_client.c"

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static int64_t clock_us;
static unsigned socket_calls, closed, select_calls;
static unsigned select_stalls;
static int64_t ready_delay = 1000;
static bool connect_pending, connect_failed, recv_cancel, alloc_failed, lock_busy, lock_replace_job;
static size_t packet_size = 17, response_size, response_pos, sent_bytes;
static uint8_t *response;
static char sent[8192];
static void *pcm_allocation;
static size_t allocation_size;
static job_t queued;
static bool has_job;
static unsigned init_fail;
static unsigned cancel_on_take;

int64_t esp_timer_get_time(void) { return clock_us; }
void *heap_caps_malloc(size_t n, unsigned caps) {
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    CHECK(n <= NM_TTS_PCM_MAX + WAV_HEADER);
    if (alloc_failed) return NULL;
    CHECK(!pcm_allocation);
    pcm_allocation = malloc(n);
    allocation_size = n;
    return pcm_allocation;
}
void heap_caps_free(void *p) {
    if (p == pcm_allocation && p) { pcm_allocation = NULL; allocation_size = 0; }
    free(p);
}
QueueHandle_t xQueueCreate(unsigned n, size_t item) {
    CHECK(n == 1 && item == sizeof(job_t));
    return init_fail == 1 ? NULL : (void *)1;
}
int xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks) {
    CHECK(q == (void *)1);
    (void)ticks;
    if (!has_job) return pdFALSE;
    memcpy(out, &queued, sizeof(queued)); has_job = false; return pdTRUE;
}
int xQueueOverwrite(QueueHandle_t q, const void *in) {
    CHECK(q == (void *)1);
    memcpy(&queued, in, sizeof(queued)); has_job = true; return pdPASS;
}
void vQueueDelete(QueueHandle_t q) { CHECK(q == (void *)1); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return init_fail == 2 ? NULL : (void *)2; }
int xSemaphoreTake(SemaphoreHandle_t q, TickType_t ticks) {
    CHECK(q == (void *)2);
    if (cancel_on_take && --cancel_on_take == 0) nm_tts_cancel();
    if (lock_replace_job && ticks) {
        lock_replace_job = false;
        CHECK(nm_tts_start("replacement after contention"));
        return pdFALSE;
    }
    return lock_busy ? pdFALSE : pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t q) { CHECK(q == (void *)2); }
void vSemaphoreDelete(SemaphoreHandle_t q) { CHECK(q == (void *)2); }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *task) {
    CHECK(fn == worker && !strcmp(name, "local_tts") && stack == 10240 && !arg && priority == 4 && !task);
    return init_fail == 3 ? pdFALSE : pdPASS;
}
int nm_fake_socket(int domain, int type, int protocol) {
    CHECK(domain == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
    ++socket_calls; return 5;
}
int nm_fake_fcntl(int fd, int op, ...) {
    CHECK(fd == 5);
    if (op == F_GETFL) return 0;
    CHECK(op == F_SETFL);
    va_list args; va_start(args, op); CHECK(va_arg(args, int) & O_NONBLOCK); va_end(args);
    return 0;
}
int nm_fake_connect(int fd, const struct sockaddr *address, socklen_t n) {
    CHECK(fd == 5 && n == sizeof(struct sockaddr_in));
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
    CHECK(v4->sin_family == AF_INET && ntohs(v4->sin_port) == 8766);
    CHECK(ntohl(v4->sin_addr.s_addr) == 0xc0a80102);
    if (connect_failed) { errno = ECONNREFUSED; return -1; }
    if (connect_pending) { errno = EINPROGRESS; return -1; }
    return 0;
}
int nm_fake_getsockopt(int fd, int level, int opt, void *value, socklen_t *n) {
    CHECK(fd == 5 && level == SOL_SOCKET && opt == SO_ERROR && *n == sizeof(int));
    *(int *)value = 0; return 0;
}
int nm_fake_select(int count, fd_set *read, fd_set *write, fd_set *error, struct timeval *tv) {
    CHECK(count == 6 && !error && ((read != NULL) != (write != NULL)));
    CHECK(tv->tv_sec == 0 && tv->tv_usec <= WAIT_US && tv->tv_usec > 0);
    ++select_calls;
    if (select_stalls) { --select_stalls; clock_us += tv->tv_usec; return 0; }
    clock_us += ready_delay;
    return 1;
}
int nm_fake_send(int fd, const void *data, size_t n, int flags) {
    CHECK(fd == 5 && !flags);
    if (n > packet_size) n = packet_size;
    CHECK(sent_bytes + n < sizeof(sent));
    memcpy(sent + sent_bytes, data, n); sent_bytes += n; sent[sent_bytes] = '\0'; return (int)n;
}
int nm_fake_recv(int fd, void *data, size_t n, int flags) {
    CHECK(fd == 5 && !flags);
    if (recv_cancel) { recv_cancel = false; nm_tts_cancel(); }
    if (n > packet_size) n = packet_size;
    if (n > response_size - response_pos) n = response_size - response_pos;
    memcpy(data, response + response_pos, n); response_pos += n; return (int)n;
}
int nm_fake_close(int fd) { CHECK(fd == 5); ++closed; return 0; }

static void put16(uint8_t *p, unsigned x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void put32(uint8_t *p, unsigned x) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (8 * i)); }
static uint8_t *make_wav(size_t samples) {
    size_t n = samples * 2 + WAV_HEADER;
    uint8_t *wav = calloc(1, n);
    memcpy(wav, "RIFF", 4); put32(wav + 4, (unsigned)n - 8);
    memcpy(wav + 8, "WAVEfmt ", 8); put32(wav + 16, 16);
    put16(wav + 20, 1); put16(wav + 22, 1); put32(wav + 24, 16000);
    put32(wav + 28, 32000); put16(wav + 32, 2); put16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); put32(wav + 40, (unsigned)samples * 2);
    for (size_t i = 0; i < samples; ++i) put16(wav + 44 + i * 2, (unsigned)(uint16_t)(int16_t)(i - 1234));
    return wav;
}
static void response_for(const char *headers, uint8_t *wav, size_t n) {
    free(response);
    size_t h = strlen(headers);
    response_size = h + n; response_pos = 0;
    response = malloc(response_size); memcpy(response, headers, h); if (n) memcpy(response + h, wav, n);
    sent_bytes = 0; sent[0] = '\0'; select_calls = 0; socket_calls = closed = 0;
    clock_us = 0; packet_size = 17; select_stalls = 0;
    ready_delay = 1000;
    cancel_on_take = 0;
    connect_pending = connect_failed = recv_cancel = alloc_failed = false;
}
static void success_response(size_t samples) {
    uint8_t *wav = make_wav(samples);
    char headers[200];
    snprintf(headers, sizeof(headers), "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", samples * 2 + 44);
    response_for(headers, wav, samples * 2 + 44); free(wav);
}
static uint32_t start(const char *text) { uint32_t id = nm_tts_start(text); CHECK(id); return id; }
static void step(void) { job_t job; CHECK(xQueueReceive(s_jobs, &job, 0)); process_job(&job); }
static nm_tts_status_t read_status(uint32_t id) { int16_t pcm[7]; size_t n; uint32_t total; return nm_tts_read(id, pcm, 7, &n, &total); }
static void reject_response(const char *header, uint8_t *wav, size_t n) {
    response_for(header, wav, n); uint32_t id = start("fixture"); step();
    CHECK(read_status(id) == NM_TTS_FAILED); CHECK(closed == socket_calls && !pcm_allocation);
}

static void test_config(void) {
    endpoint_t ep;
    const char *ok[] = { "http://10.0.0.1/v1/tts", "http://172.16.1.1:8766/v1/tts", "http://192.168.1.2/v1/tts", "http://169.254.1.2/v1/tts", "http://127.0.0.1/v1/tts" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); ++i) CHECK(endpoint_parse(ok[i], &ep));
    const char *bad[] = { "", "https://192.168.1.2/v1/tts", "http://example.com/v1/tts", "http://8.8.8.8/v1/tts", "http://192.168.01.2/v1/tts", "http://192.168.1.256/v1/tts", "http://192.168.1.2:0/v1/tts", "http://192.168.1.2:65536/v1/tts", "http://192.168.1.2/v1/tts?x", "http://user@192.168.1.2/v1/tts", "http://192.168.1.2/v1/other", "http://192.168.1.255/v1/tts", "http://192.168.1.2\r\n/v1/tts", "http://192.168.1.2:8766x/v1/tts" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) CHECK(!endpoint_parse(bad[i], &ep));
    CHECK(!token_valid("short")); CHECK(!token_valid("123456789012345\r\n")); CHECK(!token_valid("123456789012345 "));
    CHECK(token_valid(CONFIG_NEMOSSI_TTS_TOKEN));
}
static void test_pcm_and_json(void) {
    success_response(311); connect_pending = true;
    uint32_t id = start("한국어 \"quoted\" \\ newline\n");
    CHECK(socket_calls == 0 && read_status(id) == NM_TTS_WAIT); step();
    CHECK(socket_calls == 1 && closed == 1);
    char *body = strstr(sent, "\r\n\r\n"); CHECK(body); body += 4;
    cJSON *obj = cJSON_Parse(body); CHECK(obj);
    CHECK(!strcmp(cJSON_GetObjectItem(obj, "text")->valuestring, "한국어 \"quoted\" \\ newline\n")); cJSON_Delete(obj);
    CHECK(strstr(sent, "Authorization: Bearer synthetic-local-test-secret\r\n"));
    size_t offset = 0;
    while (offset < 311) {
        int16_t pcm[7]; size_t n = 99; uint32_t total = 0;
        CHECK(nm_tts_read(id, pcm, 7, &n, &total) == NM_TTS_PCM && total == 311 && n > 0 && n <= 7);
        for (size_t i = 0; i < n; ++i) CHECK(pcm[i] == (int16_t)(offset + i - 1234));
        offset += n;
    }
    CHECK(read_status(id) == NM_TTS_DONE && !pcm_allocation);
    CHECK(read_status(id) == NM_TTS_DONE);
}
static void test_http_and_wav_limits(void) {
    uint8_t *wav = make_wav(3);
    const char *bad[] = {
        "HTTP/1.1 302 Redirect\r\nContent-Type: audio/wav\r\nContent-Length: 50\r\n\r\n",
        "HTTP/1.1 401 Denied\r\nContent-Type: audio/wav\r\nContent-Length: 50\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: 50\r\nContent-Length: 50\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: 50\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: 50\r\nContent-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 50\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: 960001\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: -50\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: 44\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n\r\n",
        "HTTP/1.1 200 OK\r\n Bad: folded\r\nContent-Type: audio/wav\r\nContent-Length: 50\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) reject_response(bad[i], wav, 50);
    const unsigned invalid_offsets[] = { 0, 4, 8, 12, 16, 20, 22, 24, 28, 32, 34, 36, 40 };
    for (size_t i = 0; i < sizeof(invalid_offsets) / sizeof(invalid_offsets[0]); ++i) {
        unsigned at = invalid_offsets[i]; wav[at] ^= 1;
        reject_response("HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: 50\r\n\r\n", wav, 50);
        wav[at] ^= 1;
    }
    reject_response("HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: 50\r\n\r\n", wav, 49);
    free(wav);
    success_response(NM_TTS_PCM_MAX / 2); packet_size = 4096;
    uint32_t id = start("max"); step(); CHECK(allocation_size == 960000 && read_status(id) == NM_TTS_PCM);
    nm_tts_cancel(); discard_stale(); CHECK(!pcm_allocation);
    success_response(4); alloc_failed = true; id = start("oom"); step(); CHECK(read_status(id) == NM_TTS_FAILED && !pcm_allocation);
}
static void test_cancel_and_deadline(void) {
    success_response(200); recv_cancel = true;
    uint32_t old = start("old"); step(); CHECK(read_status(old) == NM_TTS_STALE && !pcm_allocation && closed == 1);
    success_response(200); old = start("complete but unpublished"); cancel_on_take = 2; step();
    CHECK(response_pos == response_size && closed == 1); /* All WAV bytes arrived before cancellation. */
    CHECK(read_status(old) == NM_TTS_STALE && !pcm_allocation); /* Late publish is discarded. */
    success_response(5); old = start("old queued"); uint32_t fresh = start("new queued");
    CHECK(read_status(old) == NM_TTS_STALE); step(); CHECK(read_status(fresh) == NM_TTS_PCM);
    nm_tts_cancel(); CHECK(read_status(fresh) == NM_TTS_STALE); discard_stale(); CHECK(!pcm_allocation);
    success_response(4); select_stalls = 100;
    fresh = start("timeout"); step(); CHECK(read_status(fresh) == NM_TTS_FAILED);
    CHECK(clock_us == REQUEST_US && select_calls == 30 && closed == 1);
    success_response(4); connect_failed = true;
    fresh = start("refused"); step(); CHECK(read_status(fresh) == NM_TTS_FAILED && closed == 1);
    success_response(4); fresh = start("lock"); step(); lock_busy = true;
    CHECK(read_status(fresh) == NM_TTS_WAIT); lock_busy = false;
    CHECK(read_status(fresh) == NM_TTS_PCM); nm_tts_cancel(); discard_stale();
    /* A queued new job after the worker's idle cleanup must release the old
     * full WAV before allocating another, even with no intervening reader.
     */
    success_response(4); old = start("old result"); step(); CHECK(pcm_allocation);
    success_response(5); fresh = start("replacement result"); step();
    CHECK(read_status(old) == NM_TTS_STALE && read_status(fresh) == NM_TTS_PCM);
    nm_tts_cancel(); discard_stale(); CHECK(!pcm_allocation);
    /* New request wins a mutex-contention race; the old worker cannot
     * invalidate the replacement while reporting its own failure.
     */
    success_response(4); old = start("contended"); lock_replace_job = true; step();
    fresh = queued.id; CHECK(fresh != old && read_status(fresh) == NM_TTS_WAIT);
    step(); CHECK(read_status(fresh) == NM_TTS_PCM); nm_tts_cancel(); discard_stale();
    success_response(4); packet_size = 1; ready_delay = 100000;
    fresh = start("slow progress"); step(); CHECK(read_status(fresh) == NM_TTS_FAILED);
    CHECK(clock_us >= REQUEST_US && clock_us <= REQUEST_US + ready_delay && closed == 1);
    char oversized[HEADER_MAX + 8]; memset(oversized, 'X', sizeof(oversized)); oversized[sizeof(oversized) - 1] = '\0';
    reject_response(oversized, NULL, 0);
    char huge[NM_TTS_TEXT_MAX + 2]; memset(huge, 'x', sizeof(huge)); huge[sizeof(huge) - 1] = '\0';
    CHECK(!nm_tts_start(huge) && !nm_tts_start(NULL) && !nm_tts_start(""));
    memset(huge, '\x01', NM_TTS_TEXT_MAX); huge[NM_TTS_TEXT_MAX] = '\0';
    success_response(4); fresh = start(huge); step();
    CHECK(read_status(fresh) == NM_TTS_FAILED && socket_calls == 0); /* JSON escaped body cap */
}
int main(int argc, char **argv) {
    if (argc == 2) {
        init_fail = (unsigned)atoi(argv[1]); CHECK(!nm_tts_init() && !nm_tts_enabled() && !nm_tts_start("fixture"));
    } else {
        test_config(); CHECK(nm_tts_init() && nm_tts_enabled()); CHECK(nm_tts_init());
        test_pcm_and_json(); test_http_and_wav_limits(); test_cancel_and_deadline();
    }
    nm_tts_cancel(); if (s_lock) discard_stale();
    free(response); CHECK(!pcm_allocation);
    printf("local TTS fake HTTP: %u assertions passed\n", checks);
    return 0;
}
