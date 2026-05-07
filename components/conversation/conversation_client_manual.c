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
#include <sys/types.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
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

#include "nghttp2/nghttp2.h"

#define TAG "conv_cli"

/*
 * 2026-04-30 v17 — DOWNLINK-FOCUSED VERBOSE TRACE.
 *
 * User confirmed: "上行已经稳定，关注下行：数据从哪里来、走到哪里、走了多少、
 * 收到的是不是，关键是要在一个 session 中跑完 50 轮"。
 *
 * v16 turned ON every H2 frame including uplink wire — the uplink-wire
 * trace alone produced ~1800 log lines in 12 turns and obscured the
 * downlink picture. v17 splits the macro:
 *
 *   CONV_VERBOSE_TRACE          downlink-side traces (RX H2 frames,
 *                               DATA chunks, gRPC frame slicing,
 *                               every ConversationEvent, audio_output
 *                               PCM size + sequence_number, audio_complete
 *                               summary, stream-end snapshots).
 *                               Default ON in QEMU builds because that
 *                               is exactly what the user asked to see.
 *
 *   CONV_VERBOSE_UPLINK_TRACE   per-seq uplink wire trace + per-call
 *                               ng_send_cb size etc. Default OFF.
 *                               Re-enable with -DCONV_VERBOSE_UPLINK_TRACE=1
 *                               only when actively debugging uplink
 *                               regressions.
 */
#ifndef CONV_VERBOSE_TRACE
#  if CONFIG_COLLAR_QEMU_OPENETH
#    define CONV_VERBOSE_TRACE 1
#  else
#    define CONV_VERBOSE_TRACE 0
#  endif
#endif
#ifndef CONV_VERBOSE_UPLINK_TRACE
#  define CONV_VERBOSE_UPLINK_TRACE 0
#endif

static void conv_log_heap(const char *stage)
{
    ESP_LOGW(TAG,
             "heap[%s]: free_8bit=%u largest_8bit=%u internal=%u largest_internal=%u dma=%u largest_dma=%u min_free=%u",
             stage,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}


#if CONV_VERBOSE_TRACE
#  define CONV_TRACE(fmt, ...) ESP_LOGI(TAG, "TRACE " fmt, ##__VA_ARGS__)
#else
#  define CONV_TRACE(fmt, ...) ((void)0)
#endif

static const char *h2_frame_type_str(uint8_t t)
{
    switch (t) {
    case 0x00: return "DATA";
    case 0x01: return "HEADERS";
    case 0x02: return "PRIORITY";
    case 0x03: return "RST_STREAM";
    case 0x04: return "SETTINGS";
    case 0x05: return "PUSH_PROMISE";
    case 0x06: return "PING";
    case 0x07: return "GOAWAY";
    case 0x08: return "WINDOW_UPDATE";
    case 0x09: return "CONTINUATION";
    default:   return "UNKNOWN";
    }
}


/*
 * conv_cli stack budget.
 *
 * On the QEMU OpenETH bring-up we observed a stack overflow inside
 * nghttp2_session_recv() right after the peer SETTINGS frame arrived
 * (HPACK decoder + frame dispatch + ESP_LOG temp buffers stacked deep).
 * Real-hardware 6144 words was already on the edge; bump to 8192 words
 * (32 KB) to leave a safety margin without exhausting DRAM. This is a
 * pure capacity bump - no code path changes - so it carries over to
 * real hardware safely.
 */
#define CONV_TASK_STACK_WORDS         8192
#define CONV_TASK_PRIORITY            7
#define CONV_TX_QUEUE_DEPTH           4
#define CONV_TX_AUDIO_MAX_BYTES       3200
#define CONV_TX_FRAME_MAX_BYTES       6144
#define CONV_TX_B64_MAX_BYTES         4608
#define CONV_TX_ENQUEUE_WAIT_MS       25
#define CONV_TX_DATA_SEND_TIMEOUT_MS  2000
#define CONV_SESSION_ID_MAX           64
#define CONV_RX_BUFFER_BYTES          32768

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
    bool downlink_active;
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
    uint32_t audio_complete_count;
    int64_t  last_downlink_rx_us;     /* monotonic timestamp of most recent
                                         audio_output OR audio_complete event;
                                         used by qemu_user_loop to detect
                                         "TTS quiet window" end-of-utterance. */
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
    bool headers_sent;
    bool data_pending;
    uint32_t stream_id;
    uint32_t next_stream_id;
    int32_t conn_remote_window;
    int32_t stream_remote_window;
    int32_t peer_initial_window;
    uint8_t rx_buf[CONV_RX_BUFFER_BYTES];
    size_t  rx_len;
    bool seen_first_event;
    bool seen_first_audio;
    bool peer_settings_seen;
    int last_sock_errno;
    uint32_t last_h2_code;
    uint32_t send_timeout_count;
    nghttp2_session *session;
    nghttp2_session_callbacks *callbacks;
    struct {
        const uint8_t *data;
        size_t len;
        size_t off;
        uint64_t seq;
    } tx_src;
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

static void h2_poll_rx(void);

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

static esp_err_t build_end_conversation_frame(const char *session_id,
                                              uint8_t *out_buf,
                                              size_t out_cap,
                                              size_t *out_len)
{
    if (session_id == NULL || session_id[0] == '\0' || out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    traini_EndConversationRequest req = traini_EndConversationRequest_init_default;
    req.session_id.funcs.encode = pb_encode_string_cb;
    req.session_id.arg = (void *)session_id;

    pb_ostream_t sizing = PB_OSTREAM_SIZING;
    if (!pb_encode(&sizing, traini_EndConversationRequest_fields, &req)) {
        return ESP_FAIL;
    }

    const size_t payload_len = sizing.bytes_written;
    const size_t frame_len = 5 + payload_len;
    if (frame_len > out_cap) {
        ESP_LOGE(TAG,
                 "EndConversation frame too large: sid_len=%u payload=%u frame=%u cap=%u",
                 (unsigned)strlen(session_id),
                 (unsigned)payload_len,
                 (unsigned)frame_len,
                 (unsigned)out_cap);
        return ESP_ERR_INVALID_SIZE;
    }

    out_buf[0] = 0;
    out_buf[1] = (uint8_t)((payload_len >> 24) & 0xff);
    out_buf[2] = (uint8_t)((payload_len >> 16) & 0xff);
    out_buf[3] = (uint8_t)((payload_len >> 8) & 0xff);
    out_buf[4] = (uint8_t)(payload_len & 0xff);

    pb_ostream_t os = pb_ostream_from_buffer(out_buf + 5, payload_len);
    req.session_id.arg = (void *)session_id;
    if (!pb_encode(&os, traini_EndConversationRequest_fields, &req)) {
        return ESP_FAIL;
    }

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

static void h2_close(const char *why)
{
    if (s_h2.session != NULL) {
        nghttp2_session_del(s_h2.session);
        s_h2.session = NULL;
    }
    if (s_h2.callbacks != NULL) {
        nghttp2_session_callbacks_del(s_h2.callbacks);
        s_h2.callbacks = NULL;
    }
    if (s_h2.sock >= 0) close(s_h2.sock);
    s_h2.sock = -1;
    s_h2.connected = false;
    s_h2.stream_open = false;
    s_h2.headers_sent = false;
    s_h2.data_pending = false;
    s_h2.rx_len = 0;
    /* Reset stream_id when closing the H2 transport. Otherwise a residual
     * non-zero stream_id from the just-closed transport would cause
     * h2_open_stream_if_needed() to (incorrectly) refuse to open a new
     * stream for the NEXT conversation, even though the new conversation
     * has its own fresh session_id and is allowed to open a brand-new
     * stream id (1, 3, 5, ... from nghttp2's perspective).
     * The "STREAM REOPEN FORBIDDEN" guard is meant to catch the
     * "server closed our stream mid-session and we'd silently spawn a
     * new sid that the server cannot associate with the same session_id"
     * race -- it is NOT meant to block a new conversation. Clearing
     * stream_id here keeps both behaviours correct. (2026-04-30 v12) */
    s_h2.stream_id = 0;
    s_h2.conn_remote_window = 65535;
    s_h2.stream_remote_window = s_h2.peer_initial_window;
    s_h2.seen_first_event = false;
    s_h2.seen_first_audio = false;
    s_h2.peer_settings_seen = false;
    memset(&s_h2.tx_src, 0, sizeof(s_h2.tx_src));
    s_conv.downlink_active = false;

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

typedef struct {
    uint32_t bytes;
} bytes_discard_counter_t;

static bool pb_decode_bytes_discard(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    (void)field;
    bytes_discard_counter_t *c = (bytes_discard_counter_t *)(*arg);
    uint8_t tmp[128];

    while (stream->bytes_left > 0) {
        size_t n = stream->bytes_left > sizeof(tmp) ? sizeof(tmp) : stream->bytes_left;
        if (!pb_read(stream, tmp, n)) return false;
        if (c != NULL) {
            c->bytes += (uint32_t)n;
        }
    }
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

static void handle_downlink_message(const uint8_t *data, size_t msg_len)
{
    traini_ConversationEvent evt = traini_ConversationEvent_init_default;
    bytes_discard_counter_t audio_discard = {0};
    char err_code[48] = {0};
    char err_msg[48] = {0};
    evt.event.error.code.funcs.decode = pb_decode_string_to_buf;
    evt.event.error.code.arg = err_code;
    evt.event.error.message.funcs.decode = pb_decode_string_to_buf;
    evt.event.error.message.arg = err_msg;
    evt.event.audio_output.audio_data.funcs.decode = pb_decode_bytes_discard;
    evt.event.audio_output.audio_data.arg = &audio_discard;

    pb_istream_t is = pb_istream_from_buffer(data, msg_len);
    if (pb_decode(&is, traini_ConversationEvent_fields, &evt)) {
        s_conv.rx_events++;
        if (!s_h2.seen_first_event) {
            s_h2.seen_first_event = true;
            if (s_conv.session_start_cb) s_conv.session_start_cb(s_conv.session_start_cb_arg);
            if (s_conv.conv_state == CONVERSATION_STATE_CONNECTING) conv_set_state(CONVERSATION_STATE_READY);
        }
        /*
         * Diagnostic: log the oneof tag of every decoded ConversationEvent so we can
         * tell from the QEMU log what the server is actually sending. The first 8
         * events plus every subsequent unrecognized one are printed; recognized
         * audio_output / audio_complete already have their own log paths below.
         *
         * Field tags we care about (from traini.proto):
         *   1   = audio_output
         *   2   = audio_complete
         *   100 = error
         * Anything else means the server is emitting a ConversationEvent variant
         * we haven't taught the firmware to handle (e.g. an extension), or pb_decode
         * accepted a frame whose oneof was unset.
         */
        /* 2026-04-30 v16: print EVERY decoded ConversationEvent so the
         * log can be replayed turn-by-turn without losing any audio_output
         * frame. Previously we capped at 8 events to keep logs small;
         * with verbose trace on we want all of them. */
        ESP_LOGI(TAG,
                 "rx event#%lu which_event=%u msg_len=%u (audio_output=%d audio_complete=%d error=%d)",
                 (unsigned long)s_conv.rx_events,
                 (unsigned)evt.which_event,
                 (unsigned)msg_len,
                 (int)traini_ConversationEvent_audio_output_tag,
                 (int)traini_ConversationEvent_audio_complete_tag,
                 (int)traini_ConversationEvent_error_tag);

        if (evt.which_event == traini_ConversationEvent_audio_output_tag) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t prev_us = s_conv.last_downlink_rx_us;
            s_conv.rx_audio_events++;
            s_conv.rx_audio_bytes += audio_discard.bytes;
            s_conv.last_downlink_rx_us = now_us;
            if (!s_h2.seen_first_audio) {
                s_h2.seen_first_audio = true;
                s_conv.downlink_active = true;
                if (s_tx_queue != NULL) {
                    xQueueReset(s_tx_queue);
                }
                if (s_conv.audio_start_cb) s_conv.audio_start_cb(s_conv.audio_start_cb_arg);
            }
            /* v17: print EVERY audio_output with seq/bytes/cumulative/inter-arrival
             * gap, so the log shows exactly when downlink stalls vs flows.
             * The Android client gates uplink on this same audio_output stream
             * so we want to see each one. The dialog_downlink listener decides
             * what to do with the PCM (decode / drop / forward); this trace is
             * one level lower (right at the gRPC->protobuf boundary). */
            const int64_t gap_ms = prev_us > 0 ? (now_us - prev_us) / 1000LL : -1;
            ESP_LOGI(TAG,
                     "downlink audio_output#%lu seq=%lld pcm=%lu cum=%lu gap_ms=%lld cb=%s",
                     (unsigned long)s_conv.rx_audio_events,
                     (long long)evt.event.audio_output.sequence_number,
                     (unsigned long)audio_discard.bytes,
                     (unsigned long)s_conv.rx_audio_bytes,
                     (long long)gap_ms,
                     s_conv.audio_output_cb ? "set" : "null");
            /* NOTE: pb_decode_bytes_discard() throws away the actual PCM bytes
             * before we get here -- s_conv.audio_output_cb cannot receive a
             * meaningful pointer/len in this build. Routing of decoded PCM
             * happens in handle_downlink_message via the discard path; the
             * higher-level dialog_downlink layer handles its own QEMU drop
             * policy. (See dialog_downlink.c on_audio_output.) */
        } else if (evt.which_event == traini_ConversationEvent_audio_complete_tag) {
            const int64_t now_us = esp_timer_get_time();
            s_h2.seen_first_audio = false;
            s_conv.downlink_active = false;
            s_conv.audio_complete_count++;
            s_conv.last_downlink_rx_us = now_us;
            /* v17: print summary so we know exactly what the just-completed
             * server turn delivered before audio_complete fired. */
            ESP_LOGI(TAG,
                     "downlink AUDIO_COMPLETE #%lu cum_bytes=%lu cum_audio_evt=%lu rx_evt=%lu",
                     (unsigned long)s_conv.audio_complete_count,
                     (unsigned long)s_conv.rx_audio_bytes,
                     (unsigned long)s_conv.rx_audio_events,
                     (unsigned long)s_conv.rx_events);
            if (s_conv.audio_complete_cb) s_conv.audio_complete_cb(s_conv.audio_complete_cb_arg);
        } else if (evt.which_event == traini_ConversationEvent_error_tag) {
            ESP_LOGW(TAG,
                     "downlink ERROR code='%s' msg='%s'",
                     err_code[0] ? err_code : "-",
                     err_msg[0] ? err_msg : "-");
            if (s_conv.error_cb) s_conv.error_cb(err_code, err_msg, s_conv.error_cb_arg);
        } else {
            ESP_LOGW(TAG,
                     "downlink UNKNOWN event tag=%u msg_len=%u",
                     (unsigned)evt.which_event, (unsigned)msg_len);
        }

    } else {
        ESP_LOGW(TAG, "downlink protobuf decode failed: msg_len=%u err=%s",
                 (unsigned)msg_len, PB_GET_ERROR(&is));
    }
}

static void handle_downlink(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return;
    }
    if (len > sizeof(s_h2.rx_buf) - s_h2.rx_len) {
        ESP_LOGW(TAG,
                 "downlink rx buffer overflow: have=%u add=%u cap=%u; dropping buffered bytes",
                 (unsigned)s_h2.rx_len,
                 (unsigned)len,
                 (unsigned)sizeof(s_h2.rx_buf));
        s_h2.rx_len = 0;
        if (len > sizeof(s_h2.rx_buf)) {
            return;
        }
    }

    memcpy(s_h2.rx_buf + s_h2.rx_len, data, len);
    s_h2.rx_len += len;

    size_t off = 0;
    while (s_h2.rx_len - off >= 5U) {
        const uint8_t compressed = s_h2.rx_buf[off];
        const uint32_t msg_len = ((uint32_t)s_h2.rx_buf[off + 1] << 24) |
                                 ((uint32_t)s_h2.rx_buf[off + 2] << 16) |
                                 ((uint32_t)s_h2.rx_buf[off + 3] << 8) |
                                 (uint32_t)s_h2.rx_buf[off + 4];
        const size_t frame_len = 5U + (size_t)msg_len;
        if (frame_len > sizeof(s_h2.rx_buf)) {
            ESP_LOGW(TAG, "downlink gRPC frame too large: %u > %u; dropping stream buffer",
                     (unsigned)frame_len, (unsigned)sizeof(s_h2.rx_buf));
            s_h2.rx_len = 0;
            return;
        }
        if (s_h2.rx_len - off < frame_len) {
            break;
        }
        if (compressed) {
            ESP_LOGW(TAG, "compressed downlink message unsupported; msg_len=%u", (unsigned)msg_len);
        } else {
            handle_downlink_message(s_h2.rx_buf + off + 5U, msg_len);
        }
        off += frame_len;
    }

    if (off > 0U) {
        s_h2.rx_len -= off;
        if (s_h2.rx_len > 0U) {
            memmove(s_h2.rx_buf, s_h2.rx_buf + off, s_h2.rx_len);
        }
    }
}

static void h2_mark_stream_ended(const char *why)
{
    s_h2.stream_open = false;
    s_conv.stream_ready = false;
    s_conv.stream_half_closed = true;
    s_conv.server_ended = true;
    s_conv.downlink_active = false;
    if (s_h2.rx_len > 0U) {
        ESP_LOGW(TAG, "stream ended with %u buffered downlink bytes discarded",
                 (unsigned)s_h2.rx_len);
        s_h2.rx_len = 0;
    }
    if (why != NULL) {
        const int64_t last_rx = s_conv.last_downlink_rx_us;
        const int64_t age_ms = last_rx > 0
            ? (esp_timer_get_time() - last_rx) / 1000LL
            : -1;
        ESP_LOGW(TAG,
                 "stream ended by server: %s sid=%u rx_evt=%lu rx_audio=%lu/%lu ac=%lu sent=%lu last_rx_age_ms=%lld pending=%d hdrs_sent=%d",
                 why, (unsigned)s_h2.stream_id,
                 (unsigned long)s_conv.rx_events,
                 (unsigned long)s_conv.rx_audio_events,
                 (unsigned long)s_conv.rx_audio_bytes,
                 (unsigned long)s_conv.audio_complete_count,
                 (unsigned long)s_conv.sent_chunks,
                 (long long)age_ms,
                 (int)s_h2.data_pending,
                 (int)s_h2.headers_sent);
    }
}


static nghttp2_ssize ng_send_cb(nghttp2_session *session,
                                const uint8_t *data,
                                size_t length,
                                int flags,
                                void *user_data)
{
    (void)session;
    (void)flags;
    (void)user_data;
    int n = send(s_h2.sock, data, length, 0);
    if (n > 0) {
        return (nghttp2_ssize)n;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    s_h2.last_sock_errno = (n < 0) ? errno : 0;
    return NGHTTP2_ERR_CALLBACK_FAILURE;
}

static nghttp2_ssize ng_recv_cb(nghttp2_session *session,
                                uint8_t *buf,
                                size_t length,
                                int flags,
                                void *user_data)
{
    (void)session;
    (void)flags;
    (void)user_data;
    int n = recv(s_h2.sock, buf, length, 0);
    if (n > 0) {
        /* No per-call trace here: ng_recv_cb fires on EVERY socket
         * readable wakeup including 1-byte TCP coalescing edge cases
         * and produces ~1500 lines per turn. The interesting "what
         * does the server send" view lives in ng_on_frame_recv_cb +
         * ng_on_data_chunk_recv_cb where we already log per H2 frame
         * and per DATA chunk. (v17) */
        return (nghttp2_ssize)n;
    }

    if (n == 0) {
        CONV_TRACE("ng_recv_cb: EOF (peer closed TCP)");
        return NGHTTP2_ERR_EOF;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    s_h2.last_sock_errno = errno;
    CONV_TRACE("ng_recv_cb: ERR errno=%d", errno);
    return NGHTTP2_ERR_CALLBACK_FAILURE;
}

static ssize_t ng_data_read_cb(nghttp2_session *session,

                               int32_t stream_id,
                               uint8_t *buf,
                               size_t length,
                               uint32_t *data_flags,
                               nghttp2_data_source *source,
                               void *user_data)
{
    (void)session;
    (void)stream_id;
    (void)source;
    (void)user_data;
    size_t left = s_h2.tx_src.len - s_h2.tx_src.off;
    size_t n = left < length ? left : length;
    if (n > 0U) {
        memcpy(buf, s_h2.tx_src.data + s_h2.tx_src.off, n);
        s_h2.tx_src.off += n;
    }
    if (s_h2.tx_src.off >= s_h2.tx_src.len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t)n;
}

static int ng_on_frame_recv_cb(nghttp2_session *session,
                               const nghttp2_frame *frame,
                               void *user_data)
{
    (void)session;
    (void)user_data;
#if CONV_VERBOSE_TRACE
    /* Print every received H2 frame with its type/flags/sid/length so the
     * log lets us reconstruct exactly what the server pushed and in what
     * order, including SETTINGS / WINDOW_UPDATE / PING / GOAWAY / RST. */
    {
        const char *kind = h2_frame_type_str(frame->hd.type);
        if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
            CONV_TRACE("h2 RX %s sid=%d flags=0x%02x len=%u inc=%u",
                       kind, (int)frame->hd.stream_id,
                       (unsigned)frame->hd.flags,
                       (unsigned)frame->hd.length,
                       (unsigned)frame->window_update.window_size_increment);
        } else if (frame->hd.type == NGHTTP2_GOAWAY) {
            CONV_TRACE("h2 RX %s last_sid=%d err=0x%x",
                       kind, (int)frame->goaway.last_stream_id,
                       (unsigned)frame->goaway.error_code);
        } else if (frame->hd.type == NGHTTP2_RST_STREAM) {
            CONV_TRACE("h2 RX %s sid=%d err=0x%x",
                       kind, (int)frame->hd.stream_id,
                       (unsigned)frame->rst_stream.error_code);
        } else if (frame->hd.type == NGHTTP2_HEADERS) {
            CONV_TRACE("h2 RX %s sid=%d flags=0x%02x len=%u END_STREAM=%d END_HEADERS=%d",
                       kind, (int)frame->hd.stream_id,
                       (unsigned)frame->hd.flags,
                       (unsigned)frame->hd.length,
                       (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) ? 1 : 0,
                       (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) ? 1 : 0);
        } else if (frame->hd.type == NGHTTP2_DATA) {
            CONV_TRACE("h2 RX %s sid=%d flags=0x%02x len=%u END_STREAM=%d",
                       kind, (int)frame->hd.stream_id,
                       (unsigned)frame->hd.flags,
                       (unsigned)frame->hd.length,
                       (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) ? 1 : 0);
        } else {
            CONV_TRACE("h2 RX %s sid=%d flags=0x%02x len=%u",
                       kind, (int)frame->hd.stream_id,
                       (unsigned)frame->hd.flags,
                       (unsigned)frame->hd.length);
        }
    }
#endif
    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
        s_h2.peer_settings_seen = true;
        ESP_LOGI(TAG, "peer SETTINGS received: nghttp2");
    } else if (frame->hd.stream_id == (int32_t)s_h2.stream_id) {
        if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
            h2_mark_stream_ended(frame->hd.type == NGHTTP2_HEADERS ? "headers-end-stream" : "data-end-stream");
        }
    } else if (frame->hd.type == NGHTTP2_GOAWAY) {
        s_h2.last_h2_code = frame->goaway.error_code;
        h2_close("goaway");
    } else if (frame->hd.type == NGHTTP2_RST_STREAM &&
               frame->hd.stream_id == (int32_t)s_h2.stream_id) {
        s_h2.last_h2_code = frame->rst_stream.error_code;
        h2_mark_stream_ended("rst-stream");
    }
    return 0;
}


static int ng_on_frame_send_cb(nghttp2_session *session,
                               const nghttp2_frame *frame,
                               void *user_data)
{
    (void)session;
    (void)user_data;
#if CONV_VERBOSE_TRACE
    {
        const char *kind = h2_frame_type_str(frame->hd.type);
        CONV_TRACE("h2 TX %s sid=%d flags=0x%02x len=%u",
                   kind, (int)frame->hd.stream_id,
                   (unsigned)frame->hd.flags,
                   (unsigned)frame->hd.length);
    }
#endif
    if (frame->hd.stream_id == (int32_t)s_h2.stream_id) {
        if (frame->hd.type == NGHTTP2_HEADERS) {
            s_h2.headers_sent = true;
        } else if (frame->hd.type == NGHTTP2_DATA) {
            s_h2.data_pending = false;
        }
    }
    return 0;
}

static int ng_on_data_chunk_recv_cb(nghttp2_session *session,
                                    uint8_t flags,
                                    int32_t stream_id,
                                    const uint8_t *data,
                                    size_t len,
                                    void *user_data)
{
    (void)flags;
    (void)user_data;
#if CONV_VERBOSE_TRACE
    /* Per-DATA-chunk: shows how nghttp2 splits a large DATA frame into
     * application chunks, plus the running gRPC-frame buffer fill level
     * BEFORE handle_downlink consumes it. */
    {
        static uint32_t s_trace_chunk_calls;
        s_trace_chunk_calls++;
        if (s_trace_chunk_calls <= 8U || (s_trace_chunk_calls % 16U) == 0U) {
            CONV_TRACE("rx DATA chunk #%lu sid=%d flags=0x%02x len=%u rx_buf_fill=%u",
                       (unsigned long)s_trace_chunk_calls,
                       (int)stream_id,
                       (unsigned)flags,
                       (unsigned)len,
                       (unsigned)s_h2.rx_len);
        }
    }
#endif
    if (stream_id == (int32_t)s_h2.stream_id && len > 0U) {
        handle_downlink(data, len);
    }

    /*
     * 2026-04-30 v9 — explicit flow-control consume.
     *
     * The default nghttp2 auto-window-update only fires when half of the
     * stream OR connection window has been "consumed" — i.e. when the
     * application has acknowledged it processed those bytes. Without an
     * explicit nghttp2_session_consume() call, nghttp2 considers the
     * bytes "still buffered by the application" forever, and never
     * generates a WINDOW_UPDATE. After ~7 audio_output frames (≈64KB,
     * one full default initial-window) the server therefore stalls and
     * audio_complete is permanently delayed — the exact symptom seen on
     * v6 turn 8, v7 turn 5 and v8 turn 1.
     *
     * Because we synchronously memcpy/decode every chunk inside this
     * callback we are safe to immediately acknowledge it. nghttp2 will
     * auto-emit WINDOW_UPDATE frames at its threshold (50% by default)
     * for both stream-level and connection-level windows.
     */
    if (session != NULL && stream_id != 0 && len > 0U) {
        (void)nghttp2_session_consume(session, stream_id, len);
    }
    return 0;
}

static int ng_error_cb(nghttp2_session *session,
                       const char *msg,
                       size_t len,
                       void *user_data)
{
    (void)session;
    (void)user_data;
    if (len > 0U) {
        ESP_LOGW(TAG, "nghttp2: %.*s", (int)len, msg);
    }
    return 0;
}

static esp_err_t h2_drive_io(int timeout_ms)
{
    if (!s_h2.connected || s_h2.session == NULL || s_h2.sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t deadline = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    do {
        int rv = nghttp2_session_recv(s_h2.session);
        if (rv != 0) {
            h2_close("nghttp2-recv-fail");
            return ESP_FAIL;
        }
        rv = nghttp2_session_send(s_h2.session);
        if (rv != 0) {
            h2_close("nghttp2-send-fail");
            return ESP_FAIL;
        }
        if (timeout_ms == 0) {
            return ESP_OK;
        }

        fd_set rfds;
        fd_set wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        bool want_read = nghttp2_session_want_read(s_h2.session);
        bool want_write = nghttp2_session_want_write(s_h2.session);
        if (want_read) FD_SET(s_h2.sock, &rfds);
        if (want_write) FD_SET(s_h2.sock, &wfds);
        if (!want_read && !want_write) {
            return ESP_OK;
        }

        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        if (left_us > 20000LL) left_us = 20000LL;
        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = (suseconds_t)left_us,
        };
        int sel = select(s_h2.sock + 1,
                         want_read ? &rfds : NULL,
                         want_write ? &wfds : NULL,
                         NULL,
                         &tv);
        if (sel < 0 && errno != EINTR) {
            s_h2.last_sock_errno = errno;
            h2_close("select-fail");
            return ESP_FAIL;
        }
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

static void h2_poll_rx(void)
{
    if (!s_h2.connected || s_h2.session == NULL || s_h2.sock < 0) return;
    (void)h2_drive_io(0);
}

static esp_err_t h2_connect_if_needed(void)
{
    if (s_h2.connected && s_h2.sock >= 0 && s_h2.session != NULL) return ESP_OK;

    /*
     * SINGLE-SESSION INVARIANT (2026-04-30):
     * Within ONE active session, do not silently reopen TCP/H2 if the
     * transport has already been opened AND THEN LOST while the session
     * was still active — the server has no way to associate the new
     * socket with the existing session_id, so any silent reconnect would
     * secretly become a new gRPC stream that the server doesn't recognise
     * (audio events would never come back, exactly the symptom seen on
     * 2026-04-30 turn 12 deadlock).
     *
     * The trigger for this guard is "transport_ready was once true AND
     * is now false within the same session". The conv_state_t struct is
     * memset-zeroed when each conversation starts (start_conversation
     * leaves session_active=true and transport_ready=false but DOES NOT
     * carry over any sticky lifetime counter). h2_close() also flips
     * transport_ready back to false. So the check has to consult a
     * within-session flag, not a lifetime counter — using the lifetime
     * connect_successes counter (v12 first attempt) trips the guard on
     * EVERY new conversation after the first, which made every turn
     * after turn 1 fail with "RECONNECT FORBIDDEN".
     *
     * To express "we lost transport mid-session", we look at
     * server_ended (set by h2_mark_stream_ended when the peer closes)
     * or stream_half_closed (h2_close sets it true). At the *start* of
     * a fresh conversation neither flag is asserted (start_conversation
     * doesn't touch them but conv_fail_active_session and h2_close both
     * leave the session inactive, and a new conversation calling
     * conversation_client_start_conversation sets session_active=true
     * with stream_half_closed/server_ended still in their last state…
     * which means we have to clear them at start_conversation too).
     * Be explicit: only refuse if BOTH session_active AND we observed a
     * server-side close during this session.
     */
    if (s_conv.session_active && s_conv.server_ended) {
        ESP_LOGE(TAG,
                 "RECONNECT FORBIDDEN: refusing silent TCP/H2 reconnect after server END_STREAM during active session sid='%s'",
                 s_conv.session_id[0] ? s_conv.session_id : "-");
        conv_set_error("reconnect-forbidden");
        return ESP_FAIL;
    }

    s_conv.connect_attempts++;
    conv_log_heap("connect-begin");
    int sock = tcp_connect_blocking(CONFIG_COLLAR_CONV_HOST, (uint16_t)CONFIG_COLLAR_CONV_PORT, 5000);
    if (sock < 0) {
        s_conv.connect_failures++;
        conv_set_error("tcp connect failed");
        conv_log_heap("tcp-connect-failed");
        return ESP_FAIL;
    }
    conv_log_heap("after-tcp-connect");

    s_h2.sock = sock;
    s_h2.connected = true;
    s_h2.stream_open = false;
    s_h2.rx_len = 0;
    s_h2.conn_remote_window = 65535;
    s_h2.stream_remote_window = s_h2.peer_initial_window;
    s_h2.peer_settings_seen = false;
    s_h2.headers_sent = false;
    s_h2.data_pending = false;

    conv_log_heap("before-nghttp2-callbacks-new");
    int rv = nghttp2_session_callbacks_new(&s_h2.callbacks);
    if (rv != 0) {
        ESP_LOGE(TAG, "nghttp2 callbacks alloc failed: %s heap=%lu",
                 nghttp2_strerror(rv), (unsigned long)esp_get_free_heap_size());
        conv_log_heap("nghttp2-callbacks-new-failed");
        h2_close("nghttp2-cb-alloc");
        s_conv.connect_failures++;
        return ESP_ERR_NO_MEM;
    }
    conv_log_heap("after-nghttp2-callbacks-new");
    nghttp2_session_callbacks_set_send_callback(s_h2.callbacks, ng_send_cb);
    nghttp2_session_callbacks_set_recv_callback(s_h2.callbacks, ng_recv_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(s_h2.callbacks, ng_on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_frame_send_callback(s_h2.callbacks, ng_on_frame_send_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(s_h2.callbacks, ng_on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_error_callback(s_h2.callbacks, ng_error_cb);

    conv_log_heap("before-nghttp2-session-new");
    rv = nghttp2_session_client_new(&s_h2.session, s_h2.callbacks, NULL);
    if (rv != 0) {
        ESP_LOGE(TAG, "nghttp2_session_client_new failed: %s heap=%lu",
                 nghttp2_strerror(rv), (unsigned long)esp_get_free_heap_size());
        conv_log_heap("nghttp2-session-new-failed");
        h2_close("nghttp2-session-new");
        s_conv.connect_failures++;
        return ESP_ERR_NO_MEM;
    }
    conv_log_heap("after-nghttp2-session-new");

    /* Advertise a generous local INITIAL_WINDOW_SIZE so the server isn't
     * stream-flow-control-limited on the downlink. The default 65535 has
     * been observed to bottleneck TTS streams (~6 audio_output frames of
     * ~10 KB each fills the stream window before the application has
     * finished consuming the bytes; if our WINDOW_UPDATE doesn't reach
     * the server in time the server stops sending and the turn never
     * completes with audio_complete - exactly the symptom seen
     * 2026-04-30 v6 turn 8). 1 MiB gives ~100x slack and costs only RAM
     * for nghttp2 internal accounting (we still use the same 32 KiB
     * application rx_buf because we drain it synchronously per chunk).
     */
    {
        nghttp2_settings_entry iv[1] = {
            { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1u * 1024u * 1024u },
        };
        rv = nghttp2_submit_settings(s_h2.session, NGHTTP2_FLAG_NONE, iv, 1);
    }
    if (rv != 0) {
        ESP_LOGE(TAG, "nghttp2_submit_settings failed: %s", nghttp2_strerror(rv));
        h2_close("nghttp2-settings");
        s_conv.connect_failures++;
        return ESP_FAIL;
    }
    /*
     * 2026-04-30 v10 — connection-level WINDOW_UPDATE is the real fix.
     *
     * SETTINGS.INITIAL_WINDOW_SIZE only governs *stream-level* windows
     * (and only takes effect for streams opened *after* the SETTINGS is
     * seen). The HTTP/2 spec mandates that the connection-level
     * receive window starts at exactly 65535 and can ONLY be enlarged
     * by an explicit WINDOW_UPDATE frame on stream 0 — there is no
     * SETTINGS knob for it.
     *
     * Symptom otherwise: server sends ~6 audio_output frames totalling
     * ~115 KB, the conn-level window becomes negative, server is
     * forced to stop emitting DATA on every stream of this connection,
     * and audio_complete is held back forever. nghttp2's default
     * "auto window update" does emit a WINDOW_UPDATE eventually, but
     * since the burst is bigger than the entire 65 KB window it
     * arrives only AFTER the server has already stalled — and on a
     * single-stream conversation the server is silent until our
     * WINDOW_UPDATE actually crosses the wire, which it can't do if
     * nghttp2 is starved of CPU at exactly that moment.
     *
     * nghttp2_session_set_local_window_size() with stream_id=0
     * synthesises the WINDOW_UPDATE for us *immediately* (it queues
     * the frame to be emitted on the very next nghttp2_session_send),
     * so by the time the server first writes audio_output our
     * advertised conn-level window is already 1 MiB. That removes the
     * burst-vs-default-window race entirely.
     */
    rv = nghttp2_session_set_local_window_size(s_h2.session,
                                               NGHTTP2_FLAG_NONE,
                                               0,
                                               1 * 1024 * 1024);
    if (rv != 0) {
        ESP_LOGW(TAG, "nghttp2_session_set_local_window_size(conn) failed: %s",
                 nghttp2_strerror(rv));
        /* non-fatal: session can still proceed with default 64 KiB
         * conn window, just at higher risk of the burst-stall above. */
    }

    /* Drive the preface/SETTINGS exchange, but treat "peer SETTINGS already
     * received" as success so we don't have to wait for select() to time
     * out (h2_drive_io otherwise loops until deadline because nghttp2
     * always wants_read on a freshly-opened session). */
    {
        const int64_t deadline_us = esp_timer_get_time() + 2000LL * 1000LL;
        esp_err_t dr = ESP_OK;
        while (true) {
            dr = h2_drive_io(50);
            if (dr != ESP_OK && dr != ESP_ERR_TIMEOUT) break;
            if (s_h2.peer_settings_seen) { dr = ESP_OK; break; }
            if (esp_timer_get_time() >= deadline_us) { dr = ESP_ERR_TIMEOUT; break; }
        }
        if (dr != ESP_OK) {
            h2_close("nghttp2-preface-send");
            s_conv.connect_failures++;
            return ESP_FAIL;
        }
    }

    s_conv.connect_successes++;
    s_conv.transport_ready = true;
    s_conv.stream_half_closed = false;
    conv_set_error("");
    ESP_LOGI(TAG, "nghttp2 connected (ok=%lu fail=%lu)",
             (unsigned long)s_conv.connect_successes,
             (unsigned long)s_conv.connect_failures);
    return ESP_OK;
}

static esp_err_t h2_open_stream_if_needed(const char *session_id)
{
    /* "Already open" means we have BOTH submitted the HEADERS frame AND
     * seen the on_frame_send_cb fire for it. */
    if (s_h2.stream_open && s_h2.headers_sent) return ESP_OK;
    /* Stream was already submitted but HEADERS still not flushed; just
     * keep pumping I/O instead of submitting the headers a second time
     * (which would create a brand-new stream id and orphan the old one). */
    if (s_h2.stream_open && !s_h2.headers_sent) {
        const int64_t deadline_us = esp_timer_get_time() + 4000LL * 1000LL;
        while (s_h2.connected && !s_h2.headers_sent) {
            esp_err_t dr = h2_drive_io(50);
            if (dr != ESP_OK && dr != ESP_ERR_TIMEOUT) return dr;
            if (esp_timer_get_time() >= deadline_us) break;
        }
        if (!s_h2.headers_sent) {
            ESP_LOGW(TAG, "nghttp2 stream HEADERS still not flushed after 4s; closing for retry");
            h2_close("headers-flush-timeout");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "nghttp2 stream opened (deferred->ready) sid=%u session=%s",
                 (unsigned)s_h2.stream_id, session_id);
        return ESP_OK;
    }

    /*
     * SINGLE-STREAM INVARIANT (2026-04-30):
     * Each conversation session owns EXACTLY one HTTP/2 stream from the
     * moment the session is started until the firmware itself closes it
     * (via EndConversation). If we land here with stream_open==false but
     * the session is still active (i.e. connect_successes>0 implies we
     * already opened the stream once), it means the SERVER closed the
     * stream from underneath us (e.g. END_STREAM after a single audio_complete).
     *
     * Silently calling nghttp2_submit_headers() to spin up sid=3,5,7,…
     * would look like "another StreamConversation RPC" to the server and
     * the existing session_id would no longer be associated, so the
     * server would never push audio events back. That was the actual
     * cause of the turn-7 deadlock observed in the v2 50-round QEMU run.
     *
     * Policy: refuse to silently re-open. Force the session to FAIL hard
     * so the owner state machine can decide whether to start a brand-new
     * conversation (which is the only legal way to get a new stream).
     */
    if (s_conv.session_active && s_conv.connect_successes > 0U && s_h2.stream_id != 0U) {
        ESP_LOGE(TAG,
                 "STREAM REOPEN FORBIDDEN: server closed sid=%u during active session sid='%s'; refusing to silently open a new H2 stream",
                 (unsigned)s_h2.stream_id,
                 s_conv.session_id[0] ? s_conv.session_id : "-");
        conv_set_error("server-closed-stream");
        return ESP_FAIL;
    }

    if (!session_id || session_id[0] == '\0') return ESP_ERR_INVALID_STATE;
    if (s_h2.session == NULL) return ESP_ERR_INVALID_STATE;

    char authority[128];
    snprintf(authority, sizeof(authority), "%s:%u", CONFIG_COLLAR_CONV_HOST, (unsigned)CONFIG_COLLAR_CONV_PORT);

    const nghttp2_nv headers[] = {
        {(uint8_t *)":method", (uint8_t *)"POST", 7, 4, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":scheme", (uint8_t *)"http", 7, 4, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":authority", (uint8_t *)authority, 10, strlen(authority), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":path", (uint8_t *)"/traini.ConversationService/StreamConversation", 5, 46, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-type", (uint8_t *)"application/grpc+proto", 12, 22, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"te", (uint8_t *)"trailers", 2, 8, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"grpc-encoding", (uint8_t *)"identity", 13, 8, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"grpc-accept-encoding", (uint8_t *)"identity", 20, 8, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"user-agent", (uint8_t *)"grpc-esp32c3-nghttp2/1.0", 10, 24, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"user-id", (uint8_t *)CONFIG_COLLAR_CONV_USER_ID, 7, strlen(CONFIG_COLLAR_CONV_USER_ID), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"session-id", (uint8_t *)session_id, 10, strlen(session_id), NGHTTP2_NV_FLAG_NONE},
    };

    s_h2.headers_sent = false;
    int32_t sid = nghttp2_submit_headers(s_h2.session,
                                         NGHTTP2_FLAG_NONE,
                                         -1,
                                         NULL,
                                         headers,
                                         sizeof(headers) / sizeof(headers[0]),
                                         NULL);
    if (sid < 0) {
        ESP_LOGE(TAG, "nghttp2_submit_headers failed: %s", nghttp2_strerror(sid));
        h2_close("submit-headers-fail");
        return ESP_FAIL;
    }

    s_h2.stream_open = true;
    s_h2.stream_id = (uint32_t)sid;
    s_h2.stream_remote_window = s_h2.peer_initial_window;
    s_h2.seen_first_event = false;
    s_h2.seen_first_audio = false;

    s_conv.stream_id = (int32_t)sid;
    s_conv.stream_ready = true;
    s_conv.server_ended = false;
    s_conv.stream_half_closed = false;
    s_conv.downlink_active = false;
    esp_err_t dr = h2_drive_io(2000);
    if (dr != ESP_OK || !s_h2.headers_sent) {
        ESP_LOGW(TAG, "nghttp2 stream open deferred: sid=%u sent=%d err=0x%x",
                 (unsigned)sid, s_h2.headers_sent, dr);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "nghttp2 stream opened sid=%u session=%s", (unsigned)sid, session_id);
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

    h2_poll_rx();
    if (s_conv.downlink_active) {
        return ESP_ERR_TIMEOUT;
    }

    s_h2.tx_src.data = s_tx_frame_buf;
    s_h2.tx_src.len = frame_len;
    s_h2.tx_src.off = 0U;
    s_h2.tx_src.seq = seq;
    s_h2.data_pending = true;

    nghttp2_data_provider prd = {
        .source = {.ptr = &s_h2.tx_src},
        .read_callback = ng_data_read_cb,
    };
    int rv = nghttp2_submit_data(s_h2.session,
                                 NGHTTP2_FLAG_NONE,
                                 (int32_t)s_h2.stream_id,
                                 &prd);
    if (rv == NGHTTP2_ERR_DATA_EXIST) {
        esp_err_t flush = h2_drive_io(CONV_TX_DATA_SEND_TIMEOUT_MS);
        if (flush != ESP_OK) {
            s_h2.data_pending = false;
            return flush;
        }
        rv = nghttp2_submit_data(s_h2.session,
                                 NGHTTP2_FLAG_NONE,
                                 (int32_t)s_h2.stream_id,
                                 &prd);
    }
    if (rv != 0) {
        s_h2.data_pending = false;
        ESP_LOGW(TAG, "nghttp2_submit_data failed: seq=%llu %s",
                 (unsigned long long)seq, nghttp2_strerror(rv));
        return ESP_FAIL;
    }

    esp_err_t dr = ESP_OK;
    const int64_t deadline = esp_timer_get_time() + ((int64_t)CONV_TX_DATA_SEND_TIMEOUT_MS * 1000LL);
    while (s_h2.data_pending && s_h2.connected && !s_conv.server_ended) {
        dr = h2_drive_io(20);
        if (dr != ESP_OK && dr != ESP_ERR_TIMEOUT) {
            s_h2.data_pending = false;
            return dr;
        }
        if (esp_timer_get_time() >= deadline) {
            s_h2.data_pending = false;
            if (seq < 10ULL || (seq % 25ULL) == 0ULL) {
                ESP_LOGW(TAG, "nghttp2 uplink backpressure: seq=%llu frame=%u",
                         (unsigned long long)seq, (unsigned)frame_len);
            }
            return ESP_ERR_TIMEOUT;
        }
    }
    if (!s_h2.connected || s_conv.server_ended) return ESP_ERR_INVALID_STATE;

#if CONV_VERBOSE_UPLINK_TRACE
    {
        const int64_t dt_ms = (esp_timer_get_time() - t0) / 1000LL;
        ESP_LOGI(TAG, "TRACE uplink wire seq=%llu raw=%u grpc_frame=%u send_ms=%lld pending=%d srv_end=%d",
                 (unsigned long long)seq,
                 (unsigned)len,
                 (unsigned)frame_len,
                 (long long)dt_ms,
                 (int)s_h2.data_pending,
                 (int)s_conv.server_ended);
    }
#else
    if (seq < 5ULL || (seq % 25ULL) == 0ULL) {

        const int64_t dt_ms = (esp_timer_get_time() - t0) / 1000LL;
        ESP_LOGI(TAG, "uplink wire: seq=%llu raw=%u grpc_frame=%u send_ms=%lld",
                 (unsigned long long)seq,
                 (unsigned)len,
                 (unsigned)frame_len,
                 (long long)dt_ms);
    }
#endif


    return ESP_OK;
}

static esp_err_t end_rpc_minimal_send(const char *session_id)
{
    uint8_t wire[1200];
    uint8_t grpc_frame[128];
    size_t grpc_frame_len = 0;
    esp_err_t br = build_end_conversation_frame(session_id,
                                                grpc_frame,
                                                sizeof(grpc_frame),
                                                &grpc_frame_len);
    if (br != ESP_OK) {
        return br;
    }

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

    if (off + 9 + grpc_frame_len > sizeof(wire)) return ESP_FAIL;
    h2_frame_header(wire + off, (uint32_t)grpc_frame_len, 0x00, 0x01, 1);
    off += 9;
    memcpy(wire + off, grpc_frame, grpc_frame_len);
    off += grpc_frame_len;

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
    s_conv.downlink_active = false;
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
    ESP_LOGI(TAG, "conversation worker started (nghttp2 stream transport)");

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

        if (!s_conv.session_active) {
            if (have_pending) {
                have_pending = false;
            }
            xQueueReset(s_tx_queue);
            if (s_h2.connected) {
                h2_close("idle-no-session");
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (s_conv.downlink_active) {
            if (have_pending) {
                ESP_LOGI(TAG, "downlink active: drop pending uplink seq=%" PRIu64,
                         s_pending_item.seq);
                have_pending = false;
            }
            xQueueReset(s_tx_queue);
            h2_poll_rx();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!have_pending) {
            if (xQueueReceive(s_tx_queue, &s_pending_item, pdMS_TO_TICKS(20)) == pdTRUE) {
                have_pending = true;
            } else {
                continue;
            }
        }

        if (h2_connect_if_needed() != ESP_OK) {
            fail_count++;
            conv_fail_active_session("connect-fail", ESP_FAIL, &have_pending);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        esp_err_t open_ret = h2_open_stream_if_needed(s_conv.session_id);
        if (open_ret == ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (open_ret != ESP_OK) {
            fail_count++;
            conv_fail_active_session("open-stream-fail", open_ret, &have_pending);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        esp_err_t r = h2_send_audio(s_pending_item.data, s_pending_item.len, s_pending_item.seq);
        if (r == ESP_OK) {
            have_pending = false;
            s_conv.sent_chunks++;
        } else if (r == ESP_ERR_TIMEOUT) {
            if (have_pending) {
                ESP_LOGW(TAG, "drop uplink seq=%" PRIu64 " after send backpressure",
                         s_pending_item.seq);
                have_pending = false;
            }
            xQueueReset(s_tx_queue);
            h2_poll_rx();
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (r == ESP_ERR_INVALID_STATE && s_conv.server_ended) {
            ESP_LOGW(TAG, "server ended stream before uplink seq=%" PRIu64,
                     s_pending_item.seq);
            conv_fail_active_session("server-ended", r, &have_pending);
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
    ESP_LOGI(TAG, "StreamConversation transport: nghttp2; EndConversation remains minimal h2c");
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
    if (!s_conv.configured || !s_conv.wifi_ready || !s_conv.session_active || s_conv.downlink_active) {
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
    s_conv.downlink_active = false;
    return ESP_OK;
}

esp_err_t conversation_client_end_session(void)
{
    s_conv.session_active = false;
    s_conv.downlink_active = false;
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
        /* Fresh sid every conversation - matches Android (BlinkyManager
         * uses "session_${System.currentTimeMillis()}" per round). The
         * server is per-session stateful and will REJECT a reused sid
         * (as the Step 1.5 firmware longrun trace showed). */
        snprintf(s_conv.session_id, sizeof(s_conv.session_id), "sess-%lld",
                 (long long)(esp_timer_get_time() / 1000LL));
    }

    ESP_LOGI(TAG, "pre-start EndConversation cleanup sid='%s'", s_conv.session_id);
    esp_err_t cleanup = end_rpc_minimal_send(s_conv.session_id);
    if (cleanup != ESP_OK) {
        ESP_LOGW(TAG,
                 "pre-start EndConversation cleanup failed sid='%s' err=0x%x",
                 s_conv.session_id,
                 cleanup);
        return cleanup;
    }
    ESP_LOGI(TAG, "pre-start EndConversation cleanup OK sid='%s'", s_conv.session_id);

    s_conv.session_active = true;
    s_conv.downlink_active = false;
    /* Clear sticky transport-loss flags from any prior conversation so
     * the SINGLE-SESSION RECONNECT FORBIDDEN guard (in
     * h2_connect_if_needed) doesn't trip on the very first connect of
     * THIS new conversation. The guard's "transport was lost mid-session"
     * signal must only consider events observed WITHIN the current
     * conversation, not carry-over state from the previous one.
     * (2026-04-30 v12b) */
    s_conv.server_ended = false;
    s_conv.stream_half_closed = false;
    s_conv.transport_ready = false;
    s_conv.stream_ready = false;
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

uint32_t conversation_client_audio_complete_count(void)
{
    return s_conv.audio_complete_count;
}

int64_t conversation_client_last_downlink_rx_us(void)
{
    return s_conv.last_downlink_rx_us;
}
