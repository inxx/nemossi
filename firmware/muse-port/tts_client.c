/* SPDX-License-Identifier: Apache-2.0
 * Original Nemossi adapter for a user-configured local Mac TTS HTTP service.
 * Uses ESP-IDF's public lwIP socket API. No DNS, redirects, text/audio logs,
 * files, automatic retries, Muse tokens, or third-party TTS service calls.
 */
#include "tts_client.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#ifndef CONFIG_NEMOSSI_TTS_URL
#define CONFIG_NEMOSSI_TTS_URL ""
#endif
#ifndef CONFIG_NEMOSSI_TTS_TOKEN
#define CONFIG_NEMOSSI_TTS_TOKEN ""
#endif

#define HEADER_MAX 4096u
#define WAV_HEADER 44u
#define REQUEST_US (15 * 1000000LL)
#define WAIT_US 500000LL

typedef struct {
    uint32_t id;
    char text[NM_TTS_TEXT_MAX + 1];
} job_t;

typedef struct {
    char host[16];
    uint32_t address; /* network byte order, no resolver */
    uint16_t port;
} endpoint_t;

static endpoint_t s_endpoint;
static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_lock;
static atomic_uint s_id;
static bool s_enabled;
static uint32_t s_result_id;
static nm_tts_status_t s_result_state;
static uint8_t *s_pcm;
static size_t s_pcm_bytes, s_read_bytes;

static bool endpoint_parse(const char *url, endpoint_t *out)
{
    if (!url || strncmp(url, "http://", 7)) return false;
    const char *p = url + 7;
    unsigned octets[4];
    size_t host_len = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned n = 0, digits = 0;
        const char *first = p;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 3) return false;
            n = n * 10 + (unsigned)(*p++ - '0');
        }
        if (!digits || n > 255 || (digits > 1 && *first == '0')) return false;
        octets[i] = n;
        if (i < 3 && *p++ != '.') return false;
    }
    host_len = (size_t)(p - (url + 7));
    /* Local networks only. Loopback is allowed for host fakes/development;
     * on the ESP32 it addresses the ESP32 itself, never the Mac.
     */
    bool local = octets[0] == 10 || octets[0] == 127 ||
        (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
        (octets[0] == 192 && octets[1] == 168) ||
        (octets[0] == 169 && octets[1] == 254);
    if (!local || octets[3] == 0 || octets[3] == 255 || host_len >= sizeof(out->host)) return false;
    unsigned port = 80;
    if (*p == ':') {
        ++p;
        unsigned digits = 0;
        port = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 5) return false;
            port = port * 10 + (unsigned)(*p++ - '0');
        }
        if (!digits || !port || port > 65535) return false;
    }
    if (strcmp(p, "/v1/tts")) return false;
    memcpy(out->host, url + 7, host_len);
    out->host[host_len] = '\0';
    out->address = htonl((octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3]);
    out->port = (uint16_t)port;
    return true;
}

static bool token_valid(const char *token)
{
    size_t n = token ? strlen(token) : 0;
    if (n < 16 || n > 256) return false;
    for (size_t i = 0; i < n; ++i) {
        if ((unsigned char)token[i] < 0x21 || (unsigned char)token[i] > 0x7e) return false;
    }
    return true;
}

static bool current(uint32_t id, int64_t deadline)
{
    return id == atomic_load(&s_id) && esp_timer_get_time() < deadline;
}

/* All connect/send/recv calls are nonblocking. select is bounded and checks
 * the same absolute deadline; a slow stream cannot refresh that deadline.
 */
static bool wait_socket(int fd, bool writing, uint32_t id, int64_t deadline)
{
    while (current(id, deadline)) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return false;
        if (remaining > WAIT_US) remaining = WAIT_US;
        struct timeval tv = { .tv_sec = (long)(remaining / 1000000), .tv_usec = (long)(remaining % 1000000) };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        int n = select(fd + 1, writing ? NULL : &fds, writing ? &fds : NULL, NULL, &tv);
        if (n > 0) return current(id, deadline);
        if (n < 0 && errno != EINTR) return false;
    }
    return false;
}

static bool send_all(int fd, const char *data, size_t n, uint32_t id, int64_t deadline)
{
    while (n && current(id, deadline)) {
        if (!wait_socket(fd, true, id, deadline)) return false;
        int sent = send(fd, data, n, 0);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (sent <= 0 || (size_t)sent > n) return false;
        data += sent;
        n -= (size_t)sent;
    }
    return !n && current(id, deadline);
}

static int receive(int fd, void *data, size_t n, uint32_t id, int64_t deadline)
{
    while (wait_socket(fd, false, id, deadline)) {
        int got = recv(fd, data, n, 0);
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        return got;
    }
    return -1;
}

static bool decimal_size(const char *p, size_t *size)
{
    size_t n = 0;
    if (!*p) return false;
    while (*p) {
        if (*p < '0' || *p > '9' || n > (NM_TTS_PCM_MAX + WAV_HEADER) / 10) return false;
        n = n * 10 + (unsigned)(*p++ - '0');
        if (n > NM_TTS_PCM_MAX + WAV_HEADER) return false;
    }
    *size = n;
    return n > WAV_HEADER;
}

/* Mutates a bounded, NUL-terminated header block. Rejects ambiguous framing
 * rather than supporting chunking, compression, redirects, or extra WAV chunks.
 */
static bool response_headers(char *header, size_t *bytes)
{
    char *line_end = strstr(header, "\r\n");
    if (!line_end) return false;
    *line_end = '\0';
    if (strncmp(header, "HTTP/1.0 200 ", 13) && strncmp(header, "HTTP/1.1 200 ", 13)) return false;
    bool length = false, type = false;
    char *p = line_end + 2;
    while (*p) {
        line_end = strstr(p, "\r\n");
        if (!line_end) return false;
        if (line_end == p) return length && type;
        *line_end = '\0';
        char *colon = strchr(p, ':');
        if (!colon || colon == p) return false;
        for (char *k = p; k < colon; ++k) {
            if (!((*k >= 'a' && *k <= 'z') || (*k >= 'A' && *k <= 'Z') || *k == '-')) return false;
        }
        *colon++ = '\0';
        while (*colon == ' ' || *colon == '\t') ++colon;
        char *last = colon + strlen(colon);
        while (last > colon && (last[-1] == ' ' || last[-1] == '\t')) *--last = '\0';
        for (char *v = colon; *v; ++v) {
            if ((unsigned char)*v < 0x20 || (unsigned char)*v > 0x7e) return false;
        }
        if (!strcasecmp(p, "Content-Length")) {
            if (length || !decimal_size(colon, bytes)) return false;
            length = true;
        } else if (!strcasecmp(p, "Content-Type")) {
            if (type || strcasecmp(colon, "audio/wav")) return false;
            type = true;
        } else if (!strcasecmp(p, "Transfer-Encoding") ||
                   (!strcasecmp(p, "Content-Encoding") && strcasecmp(colon, "identity"))) {
            return false;
        }
        p = line_end + 2;
    }
    return false;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool wav_valid(const uint8_t *p, size_t n)
{
    return n > WAV_HEADER && n <= NM_TTS_PCM_MAX + WAV_HEADER && !(n & 1) &&
        !memcmp(p, "RIFF", 4) && le32(p + 4) == n - 8 && !memcmp(p + 8, "WAVEfmt ", 8) &&
        le32(p + 16) == 16 && le16(p + 20) == 1 && le16(p + 22) == 1 &&
        le32(p + 24) == NM_TTS_RATE && le32(p + 28) == NM_TTS_RATE * 2 &&
        le16(p + 32) == 2 && le16(p + 34) == 16 && !memcmp(p + 36, "data", 4) &&
        le32(p + 40) == n - WAV_HEADER;
}

static uint8_t *fetch(const job_t *job, size_t *pcm_bytes)
{
    uint8_t *wav = NULL;
    int fd = -1;
    cJSON *root = cJSON_CreateObject();
    char *body = NULL;
    char request[768];
    char header[HEADER_MAX + 1];
    size_t used = 0, wav_bytes = 0, have = 0;
    int64_t deadline = esp_timer_get_time() + REQUEST_US;
    if (!root || !cJSON_AddStringToObject(root, "text", job->text)) goto done;
    body = cJSON_PrintUnformatted(root);
    if (!body || strlen(body) > NM_TTS_JSON_MAX) goto done;
    int size = snprintf(request, sizeof(request),
        "POST /v1/tts HTTP/1.1\r\nHost: %s:%u\r\nAuthorization: Bearer %s\r\n"
        "Content-Type: application/json\r\nAccept: audio/wav\r\nContent-Length: %u\r\n"
        "Connection: close\r\n\r\n", s_endpoint.host, (unsigned)s_endpoint.port,
        CONFIG_NEMOSSI_TTS_TOKEN, (unsigned)strlen(body));
    if (size <= 0 || (size_t)size >= sizeof(request) || !current(job->id, deadline)) goto done;
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0 || fd >= FD_SETSIZE) goto done;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto done;
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(s_endpoint.port), .sin_addr = { .s_addr = s_endpoint.address } };
    int rc = connect(fd, (const struct sockaddr *)&address, sizeof(address));
    if (rc < 0) {
        if (errno != EINPROGRESS || !wait_socket(fd, true, job->id, deadline)) goto done;
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error) goto done;
    }
    if (!send_all(fd, request, (size_t)size, job->id, deadline) ||
        !send_all(fd, body, strlen(body), job->id, deadline)) goto done;
    /* Header+body may arrive together. Keep at most 4 KiB before the delimiter. */
    for (;;) {
        if (used == HEADER_MAX) goto done;
        int got = receive(fd, header + used, HEADER_MAX - used, job->id, deadline);
        if (got <= 0 || (size_t)got > HEADER_MAX - used) goto done;
        used += (size_t)got;
        header[used] = '\0';
        char *end = NULL;
        for (size_t i = 3; i < used; ++i) {
            if (!memcmp(header + i - 3, "\r\n\r\n", 4)) { end = header + i + 1; break; }
        }
        if (!end) continue;
        size_t prefix = (size_t)(end - header);
        if (memchr(header, '\0', prefix)) goto done;
        have = used - prefix;
        /* Preserve initial body bytes before the parser mutates its header. */
        char saved = *end;
        *end = '\0';
        bool valid = response_headers(header, &wav_bytes);
        *end = saved;
        if (!valid || have > wav_bytes) goto done;
        wav = heap_caps_malloc(wav_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!wav) goto done;
        memcpy(wav, end, have);
        break;
    }
    while (have < wav_bytes) {
        size_t room = wav_bytes - have;
        if (room > 4096) room = 4096;
        int got = receive(fd, wav + have, room, job->id, deadline);
        if (got <= 0 || (size_t)got > room) goto done;
        have += (size_t)got;
    }
    if (!current(job->id, deadline) || !wav_valid(wav, wav_bytes)) goto done;
    memmove(wav, wav + WAV_HEADER, wav_bytes - WAV_HEADER);
    *pcm_bytes = wav_bytes - WAV_HEADER;
    if (fd >= 0) close(fd);
    memset(request, 0, sizeof(request));
    cJSON_free(body);
    cJSON_Delete(root);
    return wav;
done:
    if (fd >= 0) close(fd);
    heap_caps_free(wav);
    memset(request, 0, sizeof(request));
    cJSON_free(body);
    cJSON_Delete(root);
    return NULL;
}

static void clear_result(void)
{
    heap_caps_free(s_pcm);
    s_pcm = NULL;
    s_pcm_bytes = s_read_bytes = 0;
    s_result_id = 0;
}

static void discard_stale(void)
{
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        if (s_result_id != atomic_load(&s_id)) clear_result();
        xSemaphoreGive(s_lock);
    }
}

static void process_job(job_t *job)
{
    size_t bytes = 0;
    uint8_t *pcm = NULL;
    /* Clear the previous result before allocating the next full WAV. The
     * reader's bounded memcpy is the only other owner of this mutex.
     */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_result_id != job->id) clear_result();
        xSemaphoreGive(s_lock);
        if (job->id == atomic_load(&s_id)) pcm = fetch(job, &bytes);
    }
    memset(job->text, 0, sizeof(job->text));
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (job->id == atomic_load(&s_id)) {
            clear_result();
            s_pcm = pcm;
            s_pcm_bytes = bytes;
            s_result_id = job->id;
            s_result_state = pcm ? NM_TTS_PCM : NM_TTS_FAILED;
            pcm = NULL;
        }
        xSemaphoreGive(s_lock);
    } else {
        /* Never publish success after losing ownership. Queue work again only
         * by an explicit new turn; bounded mutex contention is a failed turn.
         */
        unsigned expected = job->id;
        atomic_compare_exchange_strong(&s_id, &expected, job->id + 1);
    }
    heap_caps_free(pcm);
}

static void worker(void *arg)
{
    (void)arg;
    job_t job;
    for (;;) {
        discard_stale();
        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(200)) == pdTRUE) process_job(&job);
    }
}

bool nm_tts_init(void)
{
    if (s_jobs) return s_enabled;
    if (!endpoint_parse(CONFIG_NEMOSSI_TTS_URL, &s_endpoint) || !token_valid(CONFIG_NEMOSSI_TTS_TOKEN)) return false;
    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(1, sizeof(job_t));
    if (!s_lock || !s_jobs || xTaskCreate(worker, "local_tts", 10 * 1024, NULL, 4, NULL) != pdPASS) {
        if (s_jobs) vQueueDelete(s_jobs);
        if (s_lock) vSemaphoreDelete(s_lock);
        s_jobs = NULL;
        s_lock = NULL;
        return false;
    }
    s_enabled = true;
    return true;
}

bool nm_tts_enabled(void) { return s_enabled; }

uint32_t nm_tts_start(const char *text)
{
    if (!s_enabled || !text) return 0;
    size_t n = strnlen(text, NM_TTS_TEXT_MAX + 1);
    if (!n || n > NM_TTS_TEXT_MAX) return 0;
    job_t job = { .id = atomic_fetch_add(&s_id, 1) + 1 };
    if (!job.id) job.id = atomic_fetch_add(&s_id, 1) + 1;
    memcpy(job.text, text, n + 1);
    bool queued = xQueueOverwrite(s_jobs, &job) == pdPASS;
    memset(job.text, 0, sizeof(job.text));
    if (!queued) { nm_tts_cancel(); return 0; }
    return job.id;
}

void nm_tts_cancel(void) { atomic_fetch_add(&s_id, 1); }

nm_tts_status_t nm_tts_read(uint32_t id, int16_t *pcm, size_t capacity,
                          size_t *frames, uint32_t *total_frames)
{
    if (!frames || !total_frames) return NM_TTS_FAILED;
    *frames = 0;
    *total_frames = 0;
    if (!id || id != atomic_load(&s_id)) return NM_TTS_STALE;
    if (!pcm || !capacity || capacity > NM_TTS_PCM_MAX / 2) return NM_TTS_FAILED;
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) return NM_TTS_WAIT;
    nm_tts_status_t status = NM_TTS_WAIT;
    if (id != atomic_load(&s_id)) {
        status = NM_TTS_STALE;
    } else if (s_result_id == id) {
        status = s_result_state;
        if (status == NM_TTS_PCM) {
            *total_frames = (uint32_t)(s_pcm_bytes / 2);
            size_t n = (s_pcm_bytes - s_read_bytes) / 2;
            if (n > capacity) n = capacity;
            if (!n) {
                status = NM_TTS_DONE;
                heap_caps_free(s_pcm);
                s_pcm = NULL;
                s_result_state = NM_TTS_DONE;
            } else {
                /* Decode LE explicitly: this is also portable in host tests. */
                for (size_t i = 0; i < n; ++i) pcm[i] = (int16_t)le16(s_pcm + s_read_bytes + i * 2);
                s_read_bytes += n * 2;
                *frames = n;
            }
        } else if (status == NM_TTS_DONE) {
            *total_frames = (uint32_t)(s_pcm_bytes / 2);
        }
    }
    xSemaphoreGive(s_lock);
    return status;
}
