#include "conversation/conversation_client.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "kernel/kernel_msgbus.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "mbedtls/base64.h"

#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "traini.pb.h"

#define TAG "conv_cli"

#define CONV_TASK_STACK_WORDS         6144
#define CONV_TASK_PRIORITY            7
#define CONV_TX_QUEUE_DEPTH           4
#define CONV_TX_AUDIO_MAX_BYTES       3200
#define CONV_TX_FRAME_MAX_BYTES       6144
#define CONV_TX_B64_MAX_BYTES         4608
#define CONV_TX_ENQUEUE_WAIT_MS       25
#define CONV_SESSION_ID_MAX           64

#ifndef CONFIG_COLLAR_CONV_HOST
#  define CONFIG_COLLAR_CONV_HOST ""
#endif
#ifndef CONFIG_COLLAR_CONV_PORT
#  define CONFIG_COLLAR_CONV_PORT 50051
#endif
#ifndef CONFIG_COLLAR_CONV_USER_ID
#  define CONFIG_COLLAR_CONV_USER_ID "CollarOne"
#endif
#ifndef CONFIG_COLLAR_CONV_AUDIO_BASE64
#  define CONFIG_COLLAR_CONV_AUDIO_BASE64 0
#endif
#ifndef CONFIG_COLLAR_CONV_AUDIO_SAMPLE_RATE
#  define CONFIG_COLLAR_CONV_AUDIO_SAMPLE_RATE 16000
#endif
#ifndef CONFIG_COLLAR_CONV_AUDIO_CHANNELS
#  define CONFIG_COLLAR_CONV_AUDIO_CHANNELS 1
#endif
#ifndef CONFIG_COLLAR_CONV_AUDIO_BIT_DEPTH
#  define CONFIG_COLLAR_CONV_AUDIO_BIT_DEPTH 16
#endif
#ifndef CONFIG_COLLAR_CONV_AUDIO_ENCODING
#  define CONFIG_COLLAR_CONV_AUDIO_ENCODING "pcm16"
#endif

typedef struct {
    uint16_t len;
    uint64_t seq;
    uint8_t  data[CONV_TX_AUDIO_MAX_BYTES];
} conv_tx_item_t;

typedef struct {
    bool configured;
    bool started;
    bool wifi_ready;

    bool transport_ready;
    bool stream_ready;
    bool stream_half_closed;
    bool server_ended;
    bool session_active;
    uint16_t grpc_status;
    int32_t stream_id;
    char session_id[CONV_SESSION_ID_MAX];

    conversation_state_t conv_state;
    uint32_t sent_chunks;
    uint32_t rx_events;
    uint32_t rx_audio_events;
    uint32_t rx_audio_bytes;
    uint32_t connect_attempts;
    uint32_t connect_successes;
    uint32_t connect_failures;
    char     last_error[96];

    conversation_audio_output_cb_t audio_output_cb;
    void *audio_output_cb_arg;
    conversation_event_cb_t audio_complete_cb;
    void *audio_complete_cb_arg;
    conversation_event_cb_t audio_start_cb;
    void *audio_start_cb_arg;
    conversation_event_cb_t session_start_cb;
    void *session_start_cb_arg;
    conversation_error_cb_t error_cb;
    void *error_cb_arg;
} conv_state_t;

typedef struct {
    int sock;
    bool connected;
    bool stream_open;
    uint32_t stream_id;
    uint32_t next_stream_id;
    int32_t conn_remote_window;
    int32_t stream_remote_window;
    int32_t peer_initial_window;
    uint8_t rx_buf[2048];
    size_t  rx_len;
    bool seen_first_event;
    bool seen_first_audio;
    int last_sock_errno;
    uint32_t last_h2_code;
} manual_h2_t;

static conv_state_t s_conv;
static manual_h2_t s_h2;
static QueueHandle_t s_tx_queue;
static StaticQueue_t s_tx_queue_struct;
static uint8_t s_tx_queue_storage[CONV_TX_QUEUE_DEPTH * sizeof(conv_tx_item_t)];
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[CONV_TASK_STACK_WORDS];
static conv_tx_item_t s_pending_item;
static uint8_t s_tx_frame_buf[CONV_TX_FRAME_MAX_BYTES];
#if CONFIG_COLLAR_CONV_AUDIO_BASE64
static uint8_t s_tx_b64_buf[CONV_TX_B64_MAX_BYTES];
#endif
static volatile bool s_abort_req;
static esp_err_t s_abort_err;
static char s_abort_reason[48];

static const char *conversation_client_state_str_impl(conversation_state_t s)
{
    switch (s) {
    case CONVERSATION_STATE_IDLE: return "IDLE";
    case CONVERSATION_STATE_CONNECTING: return "CONNECTING";
    case CONVERSATION_STATE_READY: return "READY";
    case CONVERSATION_STATE_TALKING: return "TALKING";
    case CONVERSATION_STATE_WAITING_RESPONSE: return "WAITING_RESPONSE";
    default: return "?";
    }
}

static void conv_set_error(const char *err)
{
    strlcpy(s_conv.last_error, err ? err : "", sizeof(s_conv.last_error));
}

static void conv_set_state(conversation_state_t st)
{
    if (s_conv.conv_state != st) {
        ESP_LOGI(TAG, "conv-state: %s -> %s",
                 conversation_client_state_str_impl(s_conv.conv_state),
                 conversation_client_state_str_impl(st));
        s_conv.conv_state = st;
    }
}

typedef struct { const uint8_t *buf; size_t len; } bytes_ref_t;

static bool pb_encode_bytes_cb(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
    const bytes_ref_t *ref = (const bytes_ref_t *)(*arg);
    if (!pb_encode_tag_for_field(stream, field)) return false;
    return pb_encode_string(stream, ref->buf, ref->len);
}

static bool pb_encode_string_cb(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
    const char *s = (const char *)(*arg);
    if (!pb_encode_tag_for_field(stream, field)) return false;
    return pb_encode_string(stream, (const uint8_t *)s, strlen(s));
}

static esp_err_t build_audio_frame(const uint8_t *pcm, size_t len, uint64_t seq,
                                   uint8_t *out_buf, size_t out_cap, size_t *out_len)
{
    const uint8_t *audio_payload = pcm;
    size_t audio_payload_len = len;

#if CONFIG_COLLAR_CONV_AUDIO_BASE64
    size_t olen = 0;
    if (mbedtls_base64_encode(s_tx_b64_buf, sizeof(s_tx_b64_buf), &olen, pcm, len) != 0) {
        return ESP_FAIL;
    }
    audio_payload = s_tx_b64_buf;
    audio_payload_len = olen;
#endif

    traini_AudioChunk chunk = traini_AudioChunk_init_default;
    bytes_ref_t audio_ref = { .buf = audio_payload, .len = audio_payload_len };
    chunk.audio_data.funcs.encode = pb_encode_bytes_cb;
    chunk.audio_data.arg = &audio_ref;
    chunk.sequence_number = (int64_t)seq;
    /* Keep wire format aligned with Android GrpcAudioClient defaults:
     * every chunk carries format + timestamp. */
    chunk.has_format = true;
    chunk.format.sample_rate = CONFIG_COLLAR_CONV_AUDIO_SAMPLE_RATE;
    chunk.format.channels = CONFIG_COLLAR_CONV_AUDIO_CHANNELS;
    chunk.format.bit_depth = CONFIG_COLLAR_CONV_AUDIO_BIT_DEPTH;
    chunk.format.encoding.funcs.encode = pb_encode_string_cb;
    chunk.format.encoding.arg = (void *)CONFIG_COLLAR_CONV_AUDIO_ENCODING;

    chunk.has_timestamp = true;
    struct timeval tv = {0};
    if (gettimeofday(&tv, NULL) == 0) {
        /* Match Android semantics: Timestamp built from currentTimeMillis(). */
        const int64_t now_ms = ((int64_t)tv.tv_sec * 1000LL) + ((int64_t)tv.tv_usec / 1000LL);
        chunk.timestamp.seconds = now_ms / 1000LL;
        chunk.timestamp.nanos = (int32_t)((now_ms % 1000LL) * 1000000LL);
    } else {
        const int64_t now_us = esp_timer_get_time();
        chunk.timestamp.seconds = now_us / 1000000LL;
        chunk.timestamp.nanos = (int32_t)((now_us % 1000000LL) * 1000LL);
    }

    pb_ostream_t sizing = PB_OSTREAM_SIZING;
    if (!pb_encode(&sizing, traini_AudioChunk_fields, &chunk)) return ESP_FAIL;
    const size_t payload_len = sizing.bytes_written;
    const size_t frame_len = 5 + payload_len;
    if (frame_len > out_cap) {
        ESP_LOGE(TAG,
                 "audio frame too large: seq=%llu raw=%u payload=%u frame=%u cap=%u base64=%d",
                 (unsigned long long)seq,
                 (unsigned)len,
                 (unsigned)payload_len,
                 (unsigned)frame_len,
                 (unsigned)out_cap,
                 (int)CONFIG_COLLAR_CONV_AUDIO_BASE64);
        return ESP_ERR_INVALID_SIZE;
    }

    out_buf[0] = 0;
    out_buf[1] = (uint8_t)((payload_len >> 24) & 0xff);
    out_buf[2] = (uint8_t)((payload_len >> 16) & 0xff);
    out_buf[3] = (uint8_t)((payload_len >> 8) & 0xff);
    out_buf[4] = (uint8_t)(payload_len & 0xff);

    pb_ostream_t os = pb_ostream_from_buffer(out_buf + 5, payload_len);
    chunk.audio_data.arg = &audio_ref;
    if (!pb_encode(&os, traini_AudioChunk_fields, &chunk)) return ESP_FAIL;

    *out_len = frame_len;
    return ESP_OK;
}

static int hpack_put_literal_str(uint8_t *buf, const char *s, size_t n)
{
    if (n > 126) return -1;
    buf[0] = (uint8_t)n;
    memcpy(buf + 1, s, n);
    return (int)(1 + n);
}

static int hpack_put_header(uint8_t *buf, const char *name, const char *value)
{
    buf[0] = 0x00;
    int off = 1;
    int r = hpack_put_literal_str(buf + off, name, strlen(name));
    if (r < 0) return -1;
    off += r;
    r = hpack_put_literal_str(buf + off, value, strlen(value));
    if (r < 0) return -1;
    off += r;
    return off;
}

static void h2_frame_header(uint8_t *out, uint32_t length, uint8_t type,
                            uint8_t flags, uint32_t stream_id)
{
    out[0] = (uint8_t)((length >> 16) & 0xff);
    out[1] = (uint8_t)((length >> 8) & 0xff);
    out[2] = (uint8_t)(length & 0xff);
    out[3] = type;
    out[4] = flags;
    out[5] = (uint8_t)((stream_id >> 24) & 0x7f);
    out[6] = (uint8_t)((stream_id >> 16) & 0xff);
    out[7] = (uint8_t)((stream_id >> 8) & 0xff);
    out[8] = (uint8_t)(stream_id & 0xff);
}

static int tcp_connect_blocking(const char *host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || res == NULL) return -1;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    int rv = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rv != 0) {
        close(sock);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    return sock;
}

static esp_err_t h2_send_all(const uint8_t *buf, size_t len, int timeout_ms)
{
    size_t sent = 0;
    const int64_t deadline = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);

    while (sent < len) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s_h2.sock, &wfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        int sel = select(s_h2.sock + 1, NULL, &wfds, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            s_h2.last_sock_errno = errno;
            ESP_LOGW(TAG, "h2 send select failed: errno=%d", errno);
            return ESP_FAIL;
        }
        if (sel == 0) continue;

        int n = send(s_h2.sock, buf + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n == 0) {
            s_h2.last_sock_errno = 0;
            ESP_LOGW(TAG, "h2 send returned 0 (peer closed?) sent=%u/%u",
                     (unsigned)sent, (unsigned)len);
            return ESP_FAIL;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        s_h2.last_sock_errno = (n < 0) ? errno : 0;
        ESP_LOGW(TAG, "h2 send failed: n=%d errno=%d sent=%u/%u",
                 n, (n < 0) ? errno : 0, (unsigned)sent, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void h2_close(const char *why)
{
    if (s_h2.sock >= 0) close(s_h2.sock);
    s_h2.sock = -1;
    s_h2.connected = false;
    s_h2.stream_open = false;
    s_h2.rx_len = 0;
    s_h2.conn_remote_window = 65535;
    s_h2.stream_remote_window = s_h2.peer_initial_window;
    s_h2.seen_first_event = false;
    s_h2.seen_first_audio = false;

    s_conv.transport_ready = false;
    s_conv.stream_ready = false;
    s_conv.stream_half_closed = true;
    s_conv.server_ended = false;

    if (why) {
        ESP_LOGW(TAG, "h2 close: %s (sock_errno=%d h2_code=%u)",
                 why, s_h2.last_sock_errno, (unsigned)s_h2.last_h2_code);
    }
    s_h2.last_sock_errno = 0;
    s_h2.last_h2_code = 0;
}

static esp_err_t h2_send_settings_ack(void)
{
    uint8_t f[9];
    h2_frame_header(f, 0, 0x04, 0x01, 0);
    return h2_send_all(f, sizeof(f), 1000);
}

static esp_err_t h2_send_window_update(uint32_t sid, uint32_t inc)
{
    uint8_t f[13];
    h2_frame_header(f, 4, 0x08, 0, sid);
    f[9] = (uint8_t)((inc >> 24) & 0x7f);
    f[10] = (uint8_t)((inc >> 16) & 0xff);
    f[11] = (uint8_t)((inc >> 8) & 0xff);
    f[12] = (uint8_t)(inc & 0xff);
    return h2_send_all(f, sizeof(f), 1000);
}

static void h2_on_settings(const uint8_t *pl, size_t len, bool ack)
{
    if (ack) return;
    for (size_t off = 0; off + 6 <= len; off += 6) {
        uint16_t id = (uint16_t)(((uint16_t)pl[off] << 8) | pl[off + 1]);
        uint32_t v = ((uint32_t)pl[off + 2] << 24) | ((uint32_t)pl[off + 3] << 16) |
                     ((uint32_t)pl[off + 4] << 8) | (uint32_t)pl[off + 5];
        if (id == 0x4) {
            int32_t oldv = s_h2.peer_initial_window;
            s_h2.peer_initial_window = (int32_t)v;
            if (s_h2.stream_open) s_h2.stream_remote_window += ((int32_t)v - oldv);
        }
    }
    if (h2_send_settings_ack() != ESP_OK) h2_close("settings-ack-fail");
}

static void h2_on_window_update(uint32_t sid, const uint8_t *pl, size_t len)
{
    if (len < 4) return;
    uint32_t inc = ((uint32_t)pl[0] << 24) | ((uint32_t)pl[1] << 16) |
                   ((uint32_t)pl[2] << 8) | (uint32_t)pl[3];
    inc &= 0x7fffffffU;
    if (inc == 0) return;

    if (sid == 0) s_h2.conn_remote_window += (int32_t)inc;
    else if (sid == s_h2.stream_id) s_h2.stream_remote_window += (int32_t)inc;
}

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} bytes_collector_t;

static bool pb_decode_bytes_collect(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    (void)field;
    bytes_collector_t *c = (bytes_collector_t *)(*arg);
    size_t to_read = stream->bytes_left;
    if (to_read == 0) return true;
    if (c->len + to_read > c->cap) {
        size_t nc = c->cap == 0 ? to_read : c->cap;
        while (nc < c->len + to_read) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(c->buf, nc);
        if (!nb) return false;
        c->buf = nb;
        c->cap = nc;
    }
    if (!pb_read(stream, c->buf + c->len, to_read)) return false;
    c->len += to_read;
    return true;
}

static bool pb_decode_string_to_buf(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    (void)field;
    char *dst = (char *)(*arg);
    const size_t cap = 48;
    size_t n = stream->bytes_left;
    if (n >= cap) n = cap - 1;
    if (!pb_read(stream, (pb_byte_t *)dst, n)) return false;
    dst[n] = '\0';
    while (stream->bytes_left > 0) {
        uint8_t tmp[16];
        size_t k = stream->bytes_left > sizeof(tmp) ? sizeof(tmp) : stream->bytes_left;
        if (!pb_read(stream, tmp, k)) return false;
    }
    return true;
}

static void handle_downlink(const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off + 5 <= len) {
        uint8_t compressed = data[off];
        uint32_t msg_len = ((uint32_t)data[off + 1] << 24) |
                           ((uint32_t)data[off + 2] << 16) |
                           ((uint32_t)data[off + 3] << 8) |
                           (uint32_t)data[off + 4];
        if (off + 5 + msg_len > len) return;
        if (compressed) {
            off += 5 + msg_len;
            continue;
        }

        traini_ConversationEvent evt = traini_ConversationEvent_init_default;
        bytes_collector_t audio_col = {0};
        char err_code[48] = {0};
        char err_msg[48] = {0};
        evt.event.error.code.funcs.decode = pb_decode_string_to_buf;
        evt.event.error.code.arg = err_code;
        evt.event.error.message.funcs.decode = pb_decode_string_to_buf;
        evt.event.error.message.arg = err_msg;
        evt.event.audio_output.audio_data.funcs.decode = pb_decode_bytes_collect;
        evt.event.audio_output.audio_data.arg = &audio_col;

        pb_istream_t is = pb_istream_from_buffer(data + off + 5, msg_len);
        if (pb_decode(&is, traini_ConversationEvent_fields, &evt)) {
            s_conv.rx_events++;
            if (!s_h2.seen_first_event) {
                s_h2.seen_first_event = true;
                if (s_conv.session_start_cb) s_conv.session_start_cb(s_conv.session_start_cb_arg);
                if (s_conv.conv_state == CONVERSATION_STATE_CONNECTING) conv_set_state(CONVERSATION_STATE_READY);
            }
            if (evt.which_event == traini_ConversationEvent_audio_output_tag) {
                s_conv.rx_audio_events++;
                s_conv.rx_audio_bytes += (uint32_t)audio_col.len;
                if (!s_h2.seen_first_audio) {
                    s_h2.seen_first_audio = true;
                    if (s_conv.audio_start_cb) s_conv.audio_start_cb(s_conv.audio_start_cb_arg);
                }
                if (s_conv.audio_output_cb && audio_col.len > 0) {
                    s_conv.audio_output_cb(audio_col.buf,
                                           audio_col.len,
                                           evt.event.audio_output.sequence_number,
                                           s_conv.audio_output_cb_arg);
                }
            } else if (evt.which_event == traini_ConversationEvent_audio_complete_tag) {
                s_h2.seen_first_audio = false;
                if (s_conv.audio_complete_cb) s_conv.audio_complete_cb(s_conv.audio_complete_cb_arg);
            } else if (evt.which_event == traini_ConversationEvent_error_tag) {
                if (s_conv.error_cb) s_conv.error_cb(err_code, err_msg, s_conv.error_cb_arg);
            }
        }
        free(audio_col.buf);
        off += 5 + msg_len;
    }
}

static void h2_poll_rx(void)
{
    if (!s_h2.connected || s_h2.sock < 0) return;

    for (;;) {
        if (s_h2.rx_len >= sizeof(s_h2.rx_buf)) {
            h2_close("rx-overflow");
            return;
        }
        int r = recv(s_h2.sock, s_h2.rx_buf + s_h2.rx_len, sizeof(s_h2.rx_buf) - s_h2.rx_len, 0);
        if (r > 0) s_h2.rx_len += (size_t)r;
        else if (r == 0) {
            h2_close("peer-closed");
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            h2_close("recv-err");
            return;
        }
    }

    size_t off = 0;
    while (s_h2.rx_len - off >= 9) {
        const uint8_t *h = s_h2.rx_buf + off;
        uint32_t flen = ((uint32_t)h[0] << 16) | ((uint32_t)h[1] << 8) | (uint32_t)h[2];
        uint8_t type = h[3];
        uint8_t flags = h[4];
        uint32_t sid = ((uint32_t)(h[5] & 0x7f) << 24) | ((uint32_t)h[6] << 16) |
                       ((uint32_t)h[7] << 8) | (uint32_t)h[8];
        if (s_h2.rx_len - off < 9U + flen) break;
        const uint8_t *pl = h + 9;

        switch (type) {
        case 0x04:
            h2_on_settings(pl, flen, (flags & 0x01U) != 0U);
            break;
        case 0x08:
            h2_on_window_update(sid, pl, flen);
            break;
        case 0x07:
            if (flen >= 8U) {
                uint32_t ec = ((uint32_t)pl[4] << 24) | ((uint32_t)pl[5] << 16) |
                              ((uint32_t)pl[6] << 8) | (uint32_t)pl[7];
                s_h2.last_h2_code = ec;
                ESP_LOGW(TAG, "GOAWAY: sid=%" PRIu32 " err=%" PRIu32, sid, ec);
            } else {
                ESP_LOGW(TAG, "GOAWAY: malformed len=%u", (unsigned)flen);
            }
            h2_close("goaway");
            return;
        case 0x03:
            if (sid == s_h2.stream_id) {
                if (flen >= 4U) {
                    uint32_t ec = ((uint32_t)pl[0] << 24) | ((uint32_t)pl[1] << 16) |
                                  ((uint32_t)pl[2] << 8) | (uint32_t)pl[3];
                    s_h2.last_h2_code = ec;
                    ESP_LOGW(TAG, "RST_STREAM: sid=%u err=%u", (unsigned)sid, (unsigned)ec);
                } else {
                    ESP_LOGW(TAG, "RST_STREAM: sid=%u malformed len=%u",
                             (unsigned)sid, (unsigned)flen);
                }
                s_h2.stream_open = false;
                s_conv.stream_ready = false;
                s_conv.stream_half_closed = true;
                s_conv.server_ended = true;
            }
            break;
        case 0x06:
            if ((flags & 0x01U) == 0U && flen == 8U) {
                uint8_t fr[17];
                h2_frame_header(fr, 8, 0x06, 0x01, 0);
                memcpy(fr + 9, pl, 8);
                (void)h2_send_all(fr, sizeof(fr), 1000);
            }
            break;
        case 0x00:
            if (sid == s_h2.stream_id && flen > 0) {
                handle_downlink(pl, flen);
                (void)h2_send_window_update(0, flen);
                (void)h2_send_window_update(s_h2.stream_id, flen);
                if (flags & 0x01U) {
                    s_h2.stream_open = false;
                    s_conv.stream_ready = false;
                    s_conv.stream_half_closed = true;
                    s_conv.server_ended = true;
                }
            }
            break;
        default:
            break;
        }

        off += 9U + flen;
    }

    if (off > 0 && off <= s_h2.rx_len) {
        memmove(s_h2.rx_buf, s_h2.rx_buf + off, s_h2.rx_len - off);
        s_h2.rx_len -= off;
    }
}

static esp_err_t h2_connect_if_needed(void)
{
    if (s_h2.connected && s_h2.sock >= 0) return ESP_OK;

    s_conv.connect_attempts++;
    int sock = tcp_connect_blocking(CONFIG_COLLAR_CONV_HOST, (uint16_t)CONFIG_COLLAR_CONV_PORT, 5000);
    if (sock < 0) {
        s_conv.connect_failures++;
        conv_set_error("tcp connect failed");
        return ESP_FAIL;
    }

    s_h2.sock = sock;
    s_h2.connected = true;
    s_h2.stream_open = false;
    s_h2.rx_len = 0;
    s_h2.conn_remote_window = 65535;
    s_h2.stream_remote_window = s_h2.peer_initial_window;

    uint8_t wire[64];
    size_t off = 0;
    static const char PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    memcpy(wire + off, PREFACE, sizeof(PREFACE) - 1);
    off += sizeof(PREFACE) - 1;
    h2_frame_header(wire + off, 0, 0x04, 0, 0);
    off += 9;

    if (h2_send_all(wire, off, 2000) != ESP_OK) {
        h2_close("preface-send-fail");
        s_conv.connect_failures++;
        return ESP_FAIL;
    }

    s_conv.connect_successes++;
    s_conv.transport_ready = true;
    s_conv.stream_half_closed = false;
    conv_set_error("");
    ESP_LOGI(TAG, "manual h2 connected (ok=%lu fail=%lu)",
             (unsigned long)s_conv.connect_successes,
             (unsigned long)s_conv.connect_failures);
    return ESP_OK;
}

static esp_err_t h2_open_stream_if_needed(const char *session_id)
{
    if (s_h2.stream_open) return ESP_OK;
    if (!session_id || session_id[0] == '\0') return ESP_ERR_INVALID_STATE;

    uint8_t wire[640];
    size_t off = 0;
    char authority[128];
    snprintf(authority, sizeof(authority), "%s:%u", CONFIG_COLLAR_CONV_HOST, (unsigned)CONFIG_COLLAR_CONV_PORT);

    const size_t hdr_start = off;
    off += 9;
    const size_t hdrs_begin = off;
    const char *headers[][2] = {
        {":method", "POST"},
        {":scheme", "http"},
        {":authority", authority},
        {":path", "/traini.ConversationService/StreamConversation"},
        {"content-type", "application/grpc+proto"},
        {"te", "trailers"},
        {"grpc-encoding", "identity"},
        {"grpc-accept-encoding", "identity"},
        {"user-agent", "grpc-esp32c3-manual/3.0"},
        {"user-id", CONFIG_COLLAR_CONV_USER_ID},
        {"session-id", session_id},
    };

    for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); ++i) {
        int n = hpack_put_header(wire + off, headers[i][0], headers[i][1]);
        if (n < 0 || off + (size_t)n > sizeof(wire)) return ESP_FAIL;
        off += (size_t)n;
    }

    uint32_t sid = s_h2.next_stream_id;
    if (sid == 0U || (sid & 1U) == 0U) sid = 1U;
    s_h2.next_stream_id = sid + 2U;

    h2_frame_header(wire + hdr_start, (uint32_t)(off - hdrs_begin), 0x01, 0x04, sid);

    if (h2_send_all(wire, off, 2000) != ESP_OK) {
        h2_close("open-stream-fail");
        return ESP_FAIL;
    }

    s_h2.stream_open = true;
    s_h2.stream_id = sid;
    s_h2.stream_remote_window = s_h2.peer_initial_window;
    s_h2.seen_first_event = false;
    s_h2.seen_first_audio = false;

    s_conv.stream_id = (int32_t)sid;
    s_conv.stream_ready = true;
    s_conv.server_ended = false;
    s_conv.stream_half_closed = false;
    ESP_LOGI(TAG, "manual stream opened sid=%u session=%s", (unsigned)sid, session_id);
    return ESP_OK;
}

static esp_err_t h2_send_audio(const uint8_t *pcm, size_t len, uint64_t seq)
{
    const int64_t t0 = esp_timer_get_time();
    size_t frame_len = 0;
    esp_err_t br = build_audio_frame(pcm, len, seq,
                                     s_tx_frame_buf, sizeof(s_tx_frame_buf), &frame_len);
    if (br != ESP_OK) {
        ESP_LOGW(TAG, "build_audio_frame failed: seq=%llu len=%u err=0x%x",
                 (unsigned long long)seq, (unsigned)len, br);
        return br;
    }
    if (!s_h2.stream_open || s_h2.stream_id == 0U) return ESP_ERR_INVALID_STATE;

    size_t off = 0;
    while (off < frame_len) {
        h2_poll_rx();
        int32_t cw = s_h2.conn_remote_window;
        int32_t sw = s_h2.stream_remote_window;
        int32_t win = (cw < sw) ? cw : sw;
        if (win <= 0) return ESP_ERR_TIMEOUT;

        size_t chunk = frame_len - off;
        if (chunk > 16384U) chunk = 16384U;
        if (chunk > (size_t)win) chunk = (size_t)win;

        uint8_t hdr[9];
        h2_frame_header(hdr, (uint32_t)chunk, 0x00, 0x00, s_h2.stream_id);
        if (h2_send_all(hdr, sizeof(hdr), 2000) != ESP_OK ||
            h2_send_all(s_tx_frame_buf + off, chunk, 2000) != ESP_OK) {
            h2_close("data-send-fail");
            return ESP_FAIL;
        }

        off += chunk;
        s_h2.conn_remote_window -= (int32_t)chunk;
        s_h2.stream_remote_window -= (int32_t)chunk;
    }

    if ((seq % 25ULL) == 0ULL) {
        const int64_t dt_ms = (esp_timer_get_time() - t0) / 1000LL;
        ESP_LOGI(TAG, "uplink wire: seq=%llu raw=%u grpc_frame=%u send_ms=%lld",
                 (unsigned long long)seq,
                 (unsigned)len,
                 (unsigned)frame_len,
                 (long long)dt_ms);
    }

    return ESP_OK;
}

static esp_err_t end_rpc_minimal_send(const char *session_id)
{
    uint8_t wire[1200];
    size_t off = 0;
    static const char PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    memcpy(wire + off, PREFACE, sizeof(PREFACE) - 1);
    off += sizeof(PREFACE) - 1;
    h2_frame_header(wire + off, 0, 0x04, 0, 0);
    off += 9;

    char authority[128];
    snprintf(authority, sizeof(authority), "%s:%u", CONFIG_COLLAR_CONV_HOST, (unsigned)CONFIG_COLLAR_CONV_PORT);
    const size_t hdr_frame_start = off;
    off += 9;
    const size_t hdrs_begin = off;
    const char *headers[][2] = {
        {":method", "POST"},
        {":scheme", "http"},
        {":authority", authority},
        {":path", "/traini.ConversationService/EndConversation"},
        {"content-type", "application/grpc+proto"},
        {"te", "trailers"},
        {"grpc-encoding", "identity"},
        {"grpc-accept-encoding", "identity"},
        {"user-agent", "grpc-esp32c3-manual/3.0"},
        {"user-id", CONFIG_COLLAR_CONV_USER_ID},
        {"session-id", session_id},
    };
    for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); ++i) {
        int n = hpack_put_header(wire + off, headers[i][0], headers[i][1]);
        if (n < 0 || off + (size_t)n > sizeof(wire)) return ESP_FAIL;
        off += (size_t)n;
    }
    h2_frame_header(wire + hdr_frame_start, (uint32_t)(off - hdrs_begin), 0x01, 0x04, 1);

    const size_t sid_len = strlen(session_id);
    if (sid_len >= 128) return ESP_ERR_INVALID_ARG;
    const size_t pb_len = 2 + sid_len;
    const size_t data_len = 5 + pb_len;
    if (off + 9 + data_len > sizeof(wire)) return ESP_FAIL;
    h2_frame_header(wire + off, (uint32_t)data_len, 0x00, 0x01, 1);
    off += 9;
    wire[off + 0] = 0;
    wire[off + 1] = (uint8_t)((pb_len >> 24) & 0xff);
    wire[off + 2] = (uint8_t)((pb_len >> 16) & 0xff);
    wire[off + 3] = (uint8_t)((pb_len >> 8) & 0xff);
    wire[off + 4] = (uint8_t)(pb_len & 0xff);
    wire[off + 5] = 0x0A;
    wire[off + 6] = (uint8_t)sid_len;
    memcpy(wire + off + 7, session_id, sid_len);
    off += data_len;

    int sock = tcp_connect_blocking(CONFIG_COLLAR_CONV_HOST, (uint16_t)CONFIG_COLLAR_CONV_PORT, 3000);
    if (sock < 0) return ESP_FAIL;

    size_t sent = 0;
    while (sent < off) {
        int n = send(sock, wire + sent, off - sent, 0);
        if (n > 0) sent += (size_t)n;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) vTaskDelay(pdMS_TO_TICKS(5));
        else {
            close(sock);
            return ESP_FAIL;
        }
    }
    shutdown(sock, SHUT_WR);
    close(sock);
    return ESP_OK;
}

static void conv_fail_active_session(const char *reason, esp_err_t err, bool *have_pending)
{
    char sid_snapshot[CONV_SESSION_ID_MAX] = {0};
    if (s_conv.session_id[0] != '\0') {
        strlcpy(sid_snapshot, s_conv.session_id, sizeof(sid_snapshot));
    }
    if (reason == NULL) {
        reason = "session-fail";
    }
    conv_set_error(reason);
    ESP_LOGE(TAG, "session abort: %s err=0x%x -> IDLE", reason, err);
    h2_close(reason);
    s_conv.session_active = false;
    s_conv.stream_ready = false;
    s_conv.stream_half_closed = true;
    s_h2.stream_open = false;
    if (have_pending != NULL) {
        *have_pending = false;
    }
    xQueueReset(s_tx_queue);
    conv_set_state(CONVERSATION_STATE_IDLE);

    if (sid_snapshot[0] != '\0') {
        esp_err_t er = end_rpc_minimal_send(sid_snapshot);
        ESP_LOGI(TAG, "session abort: auto EndConversation sid='%s' -> %s",
                 sid_snapshot, er == ESP_OK ? "OK" : "FAIL");
    }
}

static void conv_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "conversation worker started (manual h2 persistent mode)");

    bool have_pending = false;
    uint32_t fail_count = 0;

    for (;;) {
        h2_poll_rx();

        if (!s_conv.configured) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (s_abort_req && s_conv.session_active) {
            char reason[sizeof(s_abort_reason)] = {0};
            strlcpy(reason, s_abort_reason, sizeof(reason));
            const esp_err_t cause = s_abort_err;
            s_abort_req = false;
            conv_fail_active_session(reason[0] ? reason : "abort-request", cause, &have_pending);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        } else if (s_abort_req && !s_conv.session_active) {
            s_abort_req = false;
        }

        if (!s_conv.wifi_ready) {
            if (s_h2.connected) h2_close("wifi-lost");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (h2_connect_if_needed() != ESP_OK) {
            fail_count++;
            if (s_conv.session_active) {
                conv_fail_active_session("connect-fail", ESP_FAIL, &have_pending);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (fail_count <= 3U || (fail_count % 20U) == 0U) {
                ESP_LOGW(TAG, "manual connect failed (idle) total=%lu", (unsigned long)fail_count);
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        /*
         * Keep the TCP+h2c transport resident once Wi-Fi is ready.
         * Session only controls whether we open/drive StreamConversation.
         */
        if (!s_conv.session_active) {
            if (have_pending) {
                have_pending = false;
            }
            xQueueReset(s_tx_queue);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (h2_open_stream_if_needed(s_conv.session_id) != ESP_OK) {
            fail_count++;
            conv_fail_active_session("open-stream-fail", ESP_FAIL, &have_pending);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!have_pending) {
            if (xQueueReceive(s_tx_queue, &s_pending_item, pdMS_TO_TICKS(20)) == pdTRUE) {
                have_pending = true;
            } else {
                continue;
            }
        }

        esp_err_t r = h2_send_audio(s_pending_item.data, s_pending_item.len, s_pending_item.seq);
        if (r == ESP_OK) {
            have_pending = false;
            s_conv.sent_chunks++;
        } else if (r == ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            fail_count++;
            if (fail_count <= 3U || (fail_count % 20U) == 0U) {
                ESP_LOGW(TAG, "manual send failed seq=%" PRIu64 " total=%lu",
                         s_pending_item.seq, (unsigned long)fail_count);
            }
            conv_fail_active_session("send-fail", r, &have_pending);
        }
    }
}

esp_err_t conversation_client_start(void)
{
    if (s_conv.started) return ESP_OK;
    memset(&s_conv, 0, sizeof(s_conv));
    memset(&s_h2, 0, sizeof(s_h2));
    s_h2.sock = -1;
    s_h2.next_stream_id = 1;
    s_h2.peer_initial_window = 65535;
    s_h2.conn_remote_window = 65535;
    s_h2.stream_remote_window = 65535;

    s_conv.configured = (CONFIG_COLLAR_CONV_HOST[0] != '\0');
    s_conv.conv_state = CONVERSATION_STATE_IDLE;

    s_tx_queue = xQueueCreateStatic(CONV_TX_QUEUE_DEPTH,
                                    sizeof(conv_tx_item_t),
                                    s_tx_queue_storage,
                                    &s_tx_queue_struct);
    if (!s_tx_queue) return ESP_ERR_NO_MEM;

    TaskHandle_t th = xTaskCreateStaticPinnedToCore(conv_task,
                                                    "conv_cli",
                                                    CONV_TASK_STACK_WORDS,
                                                    NULL,
                                                    CONV_TASK_PRIORITY,
                                                    s_task_stack,
                                                    &s_task_tcb,
                                                    tskNO_AFFINITY);
    if (!th) return ESP_FAIL;

    s_conv.started = true;
    ESP_LOGI(TAG, "conversation_client armed: %s:%u rate=%dHz ch=%d bits=%d enc=%s base64=%d",
             CONFIG_COLLAR_CONV_HOST,
             (unsigned)CONFIG_COLLAR_CONV_PORT,
             CONFIG_COLLAR_CONV_AUDIO_SAMPLE_RATE,
             CONFIG_COLLAR_CONV_AUDIO_CHANNELS,
             CONFIG_COLLAR_CONV_AUDIO_BIT_DEPTH,
             CONFIG_COLLAR_CONV_AUDIO_ENCODING,
             CONFIG_COLLAR_CONV_AUDIO_BASE64);
    ESP_LOGI(TAG, "manual persistent mode: no nghttp2 dependency");
    return ESP_OK;
}

void conversation_client_handle_wifi_state(kernel_wifi_state_t state)
{
    switch (state) {
    case KERNEL_WIFI_STATE_GOT_IP:
        s_conv.wifi_ready = true;
        break;
    case KERNEL_WIFI_STATE_PROVISIONING:
    case KERNEL_WIFI_STATE_CONNECTING:
    case KERNEL_WIFI_STATE_DISCONNECTED:
    case KERNEL_WIFI_STATE_FAILED:
        s_conv.wifi_ready = false;
        break;
    default:
        break;
    }
}

void conversation_client_log_status(void)
{
    ESP_LOGI(TAG,
             "status: state=%s configured=%d wifi=%d h2c=%d stream=%d session=%d id=%s attempts=%lu ok=%lu fail=%lu sent=%lu rx_evt=%lu rx_audio=%lu/%lu grpc=%u err=%s",
             conversation_client_state_str_impl(s_conv.conv_state),
             s_conv.configured,
             s_conv.wifi_ready,
             s_conv.transport_ready,
             s_conv.stream_ready,
             s_conv.session_active,
             s_conv.session_id[0] ? s_conv.session_id : "-",
             (unsigned long)s_conv.connect_attempts,
             (unsigned long)s_conv.connect_successes,
             (unsigned long)s_conv.connect_failures,
             (unsigned long)s_conv.sent_chunks,
             (unsigned long)s_conv.rx_events,
             (unsigned long)s_conv.rx_audio_events,
             (unsigned long)s_conv.rx_audio_bytes,
             (unsigned)s_conv.grpc_status,
             s_conv.last_error[0] ? s_conv.last_error : "-");
}

bool conversation_client_is_configured(void) { return s_conv.configured; }
const char *conversation_client_host(void) { return CONFIG_COLLAR_CONV_HOST; }
uint16_t conversation_client_port(void) { return (uint16_t)CONFIG_COLLAR_CONV_PORT; }
bool conversation_client_transport_ready(void) { return s_conv.transport_ready; }
bool conversation_client_stream_ready(void) { return s_conv.stream_ready; }
bool conversation_client_stream_writable(void)
{
    return s_conv.configured && s_conv.wifi_ready && s_conv.session_active && s_conv.stream_ready;
}
bool conversation_client_session_active(void) { return s_conv.session_active; }
bool conversation_client_uplink_can_accept(void)
{
    if (!conversation_client_stream_writable()) {
        return false;
    }
    if (s_tx_queue == NULL) {
        return false;
    }
    return uxQueueSpacesAvailable(s_tx_queue) > 0U;
}

const char *conversation_client_current_session_id(void)
{
    return s_conv.session_id;
}

esp_err_t conversation_client_start_session(const char *session_id)
{
    if (session_id && session_id[0] != '\0') {
        strlcpy(s_conv.session_id, session_id, sizeof(s_conv.session_id));
    } else if (s_conv.session_id[0] == '\0') {
        snprintf(s_conv.session_id, sizeof(s_conv.session_id),
                 "sess-%lld", (long long)(esp_timer_get_time() / 1000LL));
    }
    s_conv.session_active = true;
    return ESP_OK;
}

esp_err_t conversation_client_end_session(void)
{
    s_conv.session_active = false;
    s_conv.stream_ready = false;
    s_conv.stream_half_closed = true;
    s_h2.stream_open = false;
    xQueueReset(s_tx_queue);
    return ESP_OK;
}

esp_err_t conversation_client_send_audio(const uint8_t *pcm, size_t len, uint64_t seq)
{
    if (!pcm || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > CONV_TX_AUDIO_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    if (!s_conv.session_active) return ESP_ERR_INVALID_STATE;

    conv_tx_item_t item = {0};
    item.len = (uint16_t)len;
    item.seq = seq;
    memcpy(item.data, pcm, len);
    if (xQueueSend(s_tx_queue, &item, pdMS_TO_TICKS(CONV_TX_ENQUEUE_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

conversation_state_t conversation_client_get_state(void) { return s_conv.conv_state; }
const char *conversation_client_state_str(conversation_state_t s) { return conversation_client_state_str_impl(s); }

esp_err_t conversation_client_start_conversation(const char *session_id)
{
    if (!s_conv.configured) return ESP_ERR_INVALID_STATE;
    if (s_conv.conv_state != CONVERSATION_STATE_IDLE) return ESP_ERR_INVALID_STATE;
    if (session_id && session_id[0] != '\0') {
        strlcpy(s_conv.session_id, session_id, sizeof(s_conv.session_id));
    } else {
        snprintf(s_conv.session_id, sizeof(s_conv.session_id), "sess-%lld",
                 (long long)(esp_timer_get_time() / 1000LL));
    }
    s_conv.session_active = true;
    conv_set_state(CONVERSATION_STATE_CONNECTING);
    return ESP_OK;
}

esp_err_t conversation_client_start_talking(void)
{
    if (s_conv.conv_state == CONVERSATION_STATE_IDLE) {
        esp_err_t r = conversation_client_start_conversation(NULL);
        if (r != ESP_OK) return r;
    }
    if (s_conv.conv_state != CONVERSATION_STATE_CONNECTING && s_conv.conv_state != CONVERSATION_STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    conv_set_state(CONVERSATION_STATE_TALKING);
    return ESP_OK;
}

esp_err_t conversation_client_stop_talking(void)
{
    if (s_conv.conv_state != CONVERSATION_STATE_TALKING) return ESP_ERR_INVALID_STATE;
    conv_set_state(CONVERSATION_STATE_WAITING_RESPONSE);
    return ESP_OK;
}

esp_err_t conversation_client_end_conversation(void)
{
    (void)conversation_client_end_session();
    conv_set_state(CONVERSATION_STATE_IDLE);
    return ESP_OK;
}

esp_err_t conversation_client_abort_session(const char *reason, esp_err_t cause)
{
    if (!s_conv.session_active) {
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(s_abort_reason,
            (reason != NULL && reason[0] != '\0') ? reason : "abort-request",
            sizeof(s_abort_reason));
    s_abort_err = cause;
    s_abort_req = true;
    return ESP_OK;
}

esp_err_t conversation_client_send_end_rpc(const char *session_id)
{
    if (!session_id || session_id[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!s_conv.configured) return ESP_ERR_INVALID_STATE;
    return end_rpc_minimal_send(session_id);
}

void conversation_client_set_audio_output_listener(conversation_audio_output_cb_t cb, void *arg)
{ s_conv.audio_output_cb = cb; s_conv.audio_output_cb_arg = arg; }
void conversation_client_set_audio_complete_listener(conversation_event_cb_t cb, void *arg)
{ s_conv.audio_complete_cb = cb; s_conv.audio_complete_cb_arg = arg; }
void conversation_client_set_audio_start_listener(conversation_event_cb_t cb, void *arg)
{ s_conv.audio_start_cb = cb; s_conv.audio_start_cb_arg = arg; }
void conversation_client_set_session_start_listener(conversation_event_cb_t cb, void *arg)
{ s_conv.session_start_cb = cb; s_conv.session_start_cb_arg = arg; }
void conversation_client_set_error_listener(conversation_error_cb_t cb, void *arg)
{ s_conv.error_cb = cb; s_conv.error_cb_arg = arg; }
