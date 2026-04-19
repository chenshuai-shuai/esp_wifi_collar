#include "services/conversation_service.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "services/cloud_service.h"

#define CONVERSATION_SERVICE_STACK_WORDS       6144
#define CONVERSATION_SERVICE_PRIORITY          8
#define CONVERSATION_HOST_MAX_LEN              128
#define CONVERSATION_USER_ID_MAX_LEN           64
#define CONVERSATION_SESSION_ID_MAX_LEN        80
#define CONVERSATION_ENCODING_MAX_LEN          16
#define CONVERSATION_ADDR_STR_LEN              64
#define CONVERSATION_ERROR_STR_LEN             128
#define CONVERSATION_LAST_EVENT_MAX_LEN        96
#define CONVERSATION_H2_FRAME_HEADER_LEN       9
#define CONVERSATION_STREAM_PATH_MAX_LEN       96
#define CONVERSATION_HEADERS_BLOCK_MAX_LEN     512
#define CONVERSATION_MAX_FRAME_PAYLOAD_LEN     4096
#define CONVERSATION_HTTP2_PREFACE             "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define CONVERSATION_GRPC_PREFIX_LEN           5
#define CONVERSATION_AUDIO_QUEUE_LEN           8
#define CONVERSATION_AUDIO_MAX_BYTES           2048
#define CONVERSATION_DEFAULT_SESSION_PREFIX    "esp32c3"

#define HTTP2_FRAME_DATA                       0x0
#define HTTP2_FRAME_HEADERS                    0x1
#define HTTP2_FRAME_PRIORITY                   0x2
#define HTTP2_FRAME_RST_STREAM                 0x3
#define HTTP2_FRAME_SETTINGS                   0x4
#define HTTP2_FRAME_PUSH_PROMISE               0x5
#define HTTP2_FRAME_PING                       0x6
#define HTTP2_FRAME_GOAWAY                     0x7
#define HTTP2_FRAME_WINDOW_UPDATE              0x8

#define HTTP2_FLAG_END_STREAM                  0x1
#define HTTP2_FLAG_END_HEADERS                 0x4
#define HTTP2_SETTINGS_ACK_FLAG                0x1

#define GRPC_STREAM_PATH                       "/traini.ConversationService/StreamConversation"
#define GRPC_CONTENT_TYPE                      "application/grpc+proto"
#define GRPC_TE_VALUE                          "trailers"
#define GRPC_USER_AGENT                        "esp32c3-collar/0.1"

static const char *TAG = "conv_svc";

typedef enum {
    CONVERSATION_TRANSPORT_IDLE = 0,
    CONVERSATION_TRANSPORT_CONNECTING,
    CONVERSATION_TRANSPORT_READY,
    CONVERSATION_TRANSPORT_FAILED,
} conversation_transport_state_t;

typedef enum {
    CONVERSATION_STREAM_IDLE = 0,
    CONVERSATION_STREAM_OPENING,
    CONVERSATION_STREAM_OPEN,
    CONVERSATION_STREAM_CLOSED,
    CONVERSATION_STREAM_FAILED,
} conversation_grpc_stream_state_t;

typedef struct {
    uint16_t len;
    uint64_t seq;
    uint8_t data[CONVERSATION_AUDIO_MAX_BYTES];
} conversation_audio_item_t;

typedef struct {
    bool started;
    bool wifi_ready;
    bool configured;
    bool auto_session_started;
    bool server_settings_seen;
    bool response_headers_seen;
    bool response_trailers_seen;
    uint16_t port;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bit_depth;
    int sock;
    int64_t last_attempt_us;
    uint32_t next_stream_id;
    uint32_t active_stream_id;
    uint32_t transport_success_count;
    uint32_t transport_failure_count;
    uint32_t stream_open_count;
    uint32_t stream_failure_count;
    uint32_t tx_audio_chunks;
    uint32_t tx_audio_bytes;
    uint32_t rx_audio_chunks;
    uint32_t rx_audio_bytes;
    uint32_t rx_audio_complete_count;
    uint32_t rx_error_count;
    uint8_t first_frame_type;
    uint8_t first_frame_flags;
    conversation_transport_state_t transport_state;
    conversation_grpc_stream_state_t stream_state;
    char host[CONVERSATION_HOST_MAX_LEN];
    char user_id[CONVERSATION_USER_ID_MAX_LEN];
    char session_id[CONVERSATION_SESSION_ID_MAX_LEN];
    char encoding[CONVERSATION_ENCODING_MAX_LEN];
    char peer_addr[CONVERSATION_ADDR_STR_LEN];
    char last_error[CONVERSATION_ERROR_STR_LEN];
    char last_event[CONVERSATION_LAST_EVENT_MAX_LEN];
} conversation_service_state_t;

static conversation_service_state_t s_conversation;
static StaticTask_t s_conversation_tcb;
static StackType_t s_conversation_stack[CONVERSATION_SERVICE_STACK_WORDS];
static StaticQueue_t s_audio_queue_tcb;
static uint8_t s_audio_queue_storage[CONVERSATION_AUDIO_QUEUE_LEN * sizeof(conversation_audio_item_t)];
static QueueHandle_t s_audio_queue;
static StaticSemaphore_t s_audio_enqueue_mutex_tcb;
static SemaphoreHandle_t s_audio_enqueue_mutex;
static conversation_audio_item_t s_audio_enqueue_item;

static const char *conversation_uplink_base64_string(void)
{
#ifdef CONFIG_COLLAR_CONVERSATION_UPLINK_BASE64
    return "on";
#else
    return "off";
#endif
}

static const char *conversation_downlink_base64_string(void)
{
#ifdef CONFIG_COLLAR_CONVERSATION_DOWNLINK_BASE64
    return "on";
#else
    return "off";
#endif
}

static const char *conversation_autodetect_base64_string(void)
{
#ifdef CONFIG_COLLAR_CONVERSATION_AUTODETECT_DOWNLINK_BASE64
    return "on";
#else
    return "off";
#endif
}

static void conversation_service_set_error(const char *error)
{
    strlcpy(s_conversation.last_error, error != NULL ? error : "", sizeof(s_conversation.last_error));
}

static void conversation_service_set_event(const char *event)
{
    strlcpy(s_conversation.last_event, event != NULL ? event : "", sizeof(s_conversation.last_event));
}

static void conversation_service_close_socket(void)
{
    if (s_conversation.sock >= 0) {
        close(s_conversation.sock);
        s_conversation.sock = -1;
    }
}

static const char *conversation_frame_type_string(uint8_t type)
{
    switch (type) {
    case HTTP2_FRAME_DATA:
        return "data";
    case HTTP2_FRAME_HEADERS:
        return "headers";
    case HTTP2_FRAME_PRIORITY:
        return "priority";
    case HTTP2_FRAME_RST_STREAM:
        return "rst_stream";
    case HTTP2_FRAME_SETTINGS:
        return "settings";
    case HTTP2_FRAME_PUSH_PROMISE:
        return "push_promise";
    case HTTP2_FRAME_PING:
        return "ping";
    case HTTP2_FRAME_GOAWAY:
        return "goaway";
    case HTTP2_FRAME_WINDOW_UPDATE:
        return "window_update";
    default:
        return "unknown";
    }
}

static const char *conversation_transport_state_string(void)
{
    switch (s_conversation.transport_state) {
    case CONVERSATION_TRANSPORT_IDLE:
        return "wait_transport";
    case CONVERSATION_TRANSPORT_CONNECTING:
        return "transport_connecting";
    case CONVERSATION_TRANSPORT_READY:
        return "transport_ready";
    case CONVERSATION_TRANSPORT_FAILED:
        return "transport_failed";
    default:
        return "unknown";
    }
}

static const char *conversation_stream_state_string(void)
{
    switch (s_conversation.stream_state) {
    case CONVERSATION_STREAM_IDLE:
        return "wait_stream";
    case CONVERSATION_STREAM_OPENING:
        return "stream_opening";
    case CONVERSATION_STREAM_OPEN:
        return "stream_open";
    case CONVERSATION_STREAM_CLOSED:
        return "stream_closed";
    case CONVERSATION_STREAM_FAILED:
        return "stream_failed";
    default:
        return "unknown";
    }
}

static const char *conversation_service_state_string(void)
{
    if (!CONFIG_COLLAR_CONVERSATION_ENABLE) {
        return "disabled";
    }

    if (!s_conversation.configured) {
        return "unconfigured";
    }

    if (!s_conversation.wifi_ready) {
        return "wait_wifi";
    }

    if (!cloud_service_is_reachable()) {
        if (cloud_service_has_attempted_probe() &&
            strcmp(cloud_service_host(), s_conversation.host) == 0 &&
            cloud_service_port() == s_conversation.port) {
            return "tcp_failed";
        }

        return "wait_tcp_probe";
    }

    if (strcmp(cloud_service_host(), s_conversation.host) != 0 ||
        cloud_service_port() != s_conversation.port) {
        return "probe_mismatch";
    }

    if (!CONFIG_COLLAR_CONVERSATION_TRANSPORT_ENABLE) {
        return "tcp_ready";
    }

    if (s_conversation.transport_state != CONVERSATION_TRANSPORT_READY) {
        return conversation_transport_state_string();
    }

    return conversation_stream_state_string();
}

static void conversation_service_reset_stream_state(bool clear_session)
{
    s_conversation.stream_state = CONVERSATION_STREAM_IDLE;
    s_conversation.active_stream_id = 0U;
    s_conversation.response_headers_seen = false;
    s_conversation.response_trailers_seen = false;
    if (clear_session) {
        s_conversation.session_id[0] = '\0';
        s_conversation.auto_session_started = false;
    }
}

static void conversation_service_reset_transport_state(bool clear_session)
{
    conversation_service_close_socket();
    s_conversation.transport_state = CONVERSATION_TRANSPORT_IDLE;
    s_conversation.peer_addr[0] = '\0';
    s_conversation.first_frame_type = 0xff;
    s_conversation.first_frame_flags = 0;
    s_conversation.server_settings_seen = false;
    conversation_service_reset_stream_state(clear_session);
}

static esp_err_t conversation_service_connect_with_timeout(int sock, const struct sockaddr *addr,
                                                           socklen_t addr_len, int timeout_ms)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return ESP_FAIL;
    }

    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        return ESP_FAIL;
    }

    int ret = connect(sock, addr, addr_len);
    if (ret == 0) {
        (void)fcntl(sock, F_SETFL, flags);
        return ESP_OK;
    }

    if (errno != EINPROGRESS) {
        (void)fcntl(sock, F_SETFL, flags);
        return ESP_FAIL;
    }

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);

    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    ret = select(sock + 1, NULL, &writefds, NULL, &timeout);
    if (ret <= 0) {
        (void)fcntl(sock, F_SETFL, flags);
        errno = (ret == 0) ? ETIMEDOUT : errno;
        return (ret == 0) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    int so_error = 0;
    socklen_t opt_len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &opt_len) != 0) {
        (void)fcntl(sock, F_SETFL, flags);
        return ESP_FAIL;
    }

    (void)fcntl(sock, F_SETFL, flags);
    if (so_error != 0) {
        errno = so_error;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t conversation_service_send_all(int sock, const uint8_t *data, size_t len)
{
    size_t sent = 0U;

    while (sent < len) {
        ssize_t chunk = send(sock, data + sent, len - sent, 0);
        if (chunk <= 0) {
            return ESP_FAIL;
        }
        sent += (size_t)chunk;
    }

    return ESP_OK;
}

static esp_err_t conversation_service_recv_all_timeout(int sock, uint8_t *buffer, size_t len,
                                                       int timeout_ms)
{
    size_t received = 0U;
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);

    while (received < len) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            errno = ETIMEDOUT;
            return ESP_ERR_TIMEOUT;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        struct timeval timeout = {
            .tv_sec = (int)(remaining_us / 1000000LL),
            .tv_usec = (int)(remaining_us % 1000000LL),
        };

        int sel_ret = select(sock + 1, &readfds, NULL, NULL, &timeout);
        if (sel_ret <= 0) {
            errno = (sel_ret == 0) ? ETIMEDOUT : errno;
            return (sel_ret == 0) ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }

        ssize_t chunk = recv(sock, buffer + received, len - received, 0);
        if (chunk <= 0) {
            errno = (chunk == 0) ? ECONNRESET : errno;
            return ESP_FAIL;
        }

        received += (size_t)chunk;
    }

    return ESP_OK;
}

static void conversation_service_format_sockaddr(const struct sockaddr *addr, char *out, size_t out_len)
{
    if (out_len == 0U) {
        return;
    }

    out[0] = '\0';

    if (addr == NULL) {
        return;
    }

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &ipv4->sin_addr, out, out_len);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &ipv6->sin6_addr, out, out_len);
    } else {
        strlcpy(out, "unknown_af", out_len);
    }
}

static esp_err_t conversation_service_send_frame(uint8_t type, uint8_t flags, uint32_t stream_id,
                                                 const uint8_t *payload, uint32_t payload_len)
{
    uint8_t header[CONVERSATION_H2_FRAME_HEADER_LEN];

    header[0] = (uint8_t)((payload_len >> 16) & 0xffU);
    header[1] = (uint8_t)((payload_len >> 8) & 0xffU);
    header[2] = (uint8_t)(payload_len & 0xffU);
    header[3] = type;
    header[4] = flags;
    header[5] = (uint8_t)((stream_id >> 24) & 0x7fU);
    header[6] = (uint8_t)((stream_id >> 16) & 0xffU);
    header[7] = (uint8_t)((stream_id >> 8) & 0xffU);
    header[8] = (uint8_t)(stream_id & 0xffU);

    esp_err_t ret = conversation_service_send_all(s_conversation.sock, header, sizeof(header));
    if (ret != ESP_OK) {
        return ret;
    }

    if (payload_len > 0U) {
        ret = conversation_service_send_all(s_conversation.sock, payload, payload_len);
    }

    return ret;
}

static size_t hpack_encode_integer(uint8_t *out, size_t out_len, uint32_t value,
                                   uint8_t prefix_bits, uint8_t first_byte_mask)
{
    if (out_len == 0U) {
        return 0U;
    }

    const uint32_t max_prefix = ((uint32_t)1U << prefix_bits) - 1U;
    size_t used = 0U;

    if (value < max_prefix) {
        out[used++] = (uint8_t)(first_byte_mask | (uint8_t)value);
        return used;
    }

    out[used++] = (uint8_t)(first_byte_mask | (uint8_t)max_prefix);
    value -= max_prefix;

    while (value >= 128U && used < out_len) {
        out[used++] = (uint8_t)((value % 128U) + 128U);
        value /= 128U;
    }

    if (used >= out_len) {
        return 0U;
    }

    out[used++] = (uint8_t)value;
    return used;
}

static size_t hpack_encode_string(uint8_t *out, size_t out_len, const char *text)
{
    const size_t len = strlen(text);
    size_t used = hpack_encode_integer(out, out_len, (uint32_t)len, 7, 0x00);
    if (used == 0U || (used + len) > out_len) {
        return 0U;
    }

    memcpy(out + used, text, len);
    used += len;
    return used;
}

static size_t hpack_append_indexed(uint8_t *out, size_t out_len, uint32_t index)
{
    return hpack_encode_integer(out, out_len, index, 7, 0x80);
}

static size_t hpack_append_literal_indexed_name(uint8_t *out, size_t out_len, uint32_t name_index,
                                                const char *value)
{
    size_t used = hpack_encode_integer(out, out_len, name_index, 4, 0x00);
    size_t chunk;
    if (used == 0U) {
        return 0U;
    }

    chunk = hpack_encode_string(out + used, out_len - used, value);
    if (chunk == 0U) {
        return 0U;
    }

    return used + chunk;
}

static size_t hpack_append_literal_new_name(uint8_t *out, size_t out_len,
                                            const char *name, const char *value)
{
    size_t used = hpack_encode_integer(out, out_len, 0U, 4, 0x00);
    size_t chunk;

    if (used == 0U) {
        return 0U;
    }

    chunk = hpack_encode_string(out + used, out_len - used, name);
    if (chunk == 0U) {
        return 0U;
    }
    used += chunk;

    chunk = hpack_encode_string(out + used, out_len - used, value);
    if (chunk == 0U) {
        return 0U;
    }
    used += chunk;

    return used;
}

static esp_err_t conversation_service_send_settings_ack(int sock)
{
    static const uint8_t settings_ack_frame[CONVERSATION_H2_FRAME_HEADER_LEN] = {
        0x00, 0x00, 0x00,
        HTTP2_FRAME_SETTINGS,
        HTTP2_SETTINGS_ACK_FLAG,
        0x00, 0x00, 0x00, 0x00,
    };

    return conversation_service_send_all(sock,
                                         settings_ack_frame,
                                         sizeof(settings_ack_frame));
}

static bool conversation_service_transport_due(int64_t now_us)
{
    const int64_t retry_interval_us =
        (int64_t)CONFIG_COLLAR_CONVERSATION_TRANSPORT_RETRY_INTERVAL_SEC * 1000000LL;

    if (s_conversation.last_attempt_us == 0) {
        return true;
    }

    return (now_us - s_conversation.last_attempt_us) >= retry_interval_us;
}

static esp_err_t conversation_service_open_transport(void)
{
    char port_str[8];
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    struct addrinfo *entry = NULL;
    int gai_ret;

    s_conversation.last_attempt_us = esp_timer_get_time();
    s_conversation.transport_state = CONVERSATION_TRANSPORT_CONNECTING;
    s_conversation.peer_addr[0] = '\0';
    s_conversation.first_frame_type = 0xff;
    s_conversation.first_frame_flags = 0;
    s_conversation.server_settings_seen = false;
    conversation_service_set_error("");

    snprintf(port_str, sizeof(port_str), "%u", (unsigned int)s_conversation.port);
    gai_ret = getaddrinfo(s_conversation.host, port_str, &hints, &result);
    if (gai_ret != 0 || result == NULL) {
        s_conversation.transport_state = CONVERSATION_TRANSPORT_FAILED;
        s_conversation.transport_failure_count++;
        snprintf(s_conversation.last_error, sizeof(s_conversation.last_error), "getaddrinfo=%d", gai_ret);
        return ESP_FAIL;
    }

    for (entry = result; entry != NULL; entry = entry->ai_next) {
        uint8_t frame_header[CONVERSATION_H2_FRAME_HEADER_LEN];
        int sock = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (sock < 0) {
            continue;
        }

        conversation_service_format_sockaddr(entry->ai_addr, s_conversation.peer_addr,
                                             sizeof(s_conversation.peer_addr));

        esp_err_t ret = conversation_service_connect_with_timeout(
            sock,
            entry->ai_addr,
            (socklen_t)entry->ai_addrlen,
            CONFIG_COLLAR_CONVERSATION_TRANSPORT_CONNECT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            close(sock);
            continue;
        }

        static const uint8_t client_settings_frame[CONVERSATION_H2_FRAME_HEADER_LEN] = {
            0x00, 0x00, 0x00,
            HTTP2_FRAME_SETTINGS,
            0x00,
            0x00, 0x00, 0x00, 0x00,
        };

        ret = conversation_service_send_all(
            sock,
            (const uint8_t *)CONVERSATION_HTTP2_PREFACE,
            sizeof(CONVERSATION_HTTP2_PREFACE) - 1U);
        if (ret != ESP_OK) {
            close(sock);
            continue;
        }

        ret = conversation_service_send_all(sock, client_settings_frame, sizeof(client_settings_frame));
        if (ret != ESP_OK) {
            close(sock);
            continue;
        }

        ret = conversation_service_recv_all_timeout(
            sock,
            frame_header,
            sizeof(frame_header),
            CONFIG_COLLAR_CONVERSATION_TRANSPORT_HANDSHAKE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            close(sock);
            continue;
        }

        const uint32_t payload_len =
            ((uint32_t)frame_header[0] << 16) |
            ((uint32_t)frame_header[1] << 8) |
            ((uint32_t)frame_header[2]);
        const uint8_t frame_type = frame_header[3];
        const uint8_t frame_flags = frame_header[4];

        s_conversation.first_frame_type = frame_type;
        s_conversation.first_frame_flags = frame_flags;

        if (payload_len > 0U) {
            uint8_t discard[64];
            uint32_t remaining = payload_len;

            while (remaining > 0U) {
                size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
                ret = conversation_service_recv_all_timeout(
                    sock,
                    discard,
                    chunk,
                    CONFIG_COLLAR_CONVERSATION_TRANSPORT_HANDSHAKE_TIMEOUT_MS);
                if (ret != ESP_OK) {
                    break;
                }
                remaining -= (uint32_t)chunk;
            }

            if (ret != ESP_OK) {
                close(sock);
                continue;
            }
        }

        if (frame_type == HTTP2_FRAME_SETTINGS) {
            s_conversation.server_settings_seen = true;
            if ((frame_flags & HTTP2_SETTINGS_ACK_FLAG) == 0U) {
                ret = conversation_service_send_settings_ack(sock);
                if (ret != ESP_OK) {
                    close(sock);
                    continue;
                }
            }
        } else if (frame_type != HTTP2_FRAME_WINDOW_UPDATE &&
                   frame_type != HTTP2_FRAME_PING &&
                   frame_type != HTTP2_FRAME_GOAWAY) {
            snprintf(s_conversation.last_error, sizeof(s_conversation.last_error),
                     "unexpected_frame=%u", (unsigned int)frame_type);
            close(sock);
            freeaddrinfo(result);
            s_conversation.transport_state = CONVERSATION_TRANSPORT_FAILED;
            s_conversation.transport_failure_count++;
            return ESP_FAIL;
        }

        s_conversation.sock = sock;
        s_conversation.transport_state = CONVERSATION_TRANSPORT_READY;
        s_conversation.transport_success_count++;
        s_conversation.next_stream_id = 1U;
        conversation_service_set_error("");
        freeaddrinfo(result);
        return ESP_OK;
    }

    freeaddrinfo(result);
    s_conversation.transport_state = CONVERSATION_TRANSPORT_FAILED;
    s_conversation.transport_failure_count++;

    if (s_conversation.last_error[0] == '\0') {
        if (errno != 0) {
            snprintf(s_conversation.last_error, sizeof(s_conversation.last_error),
                     "transport errno=%d", errno);
        } else {
            conversation_service_set_error("transport handshake failed");
        }
    }

    return ESP_FAIL;
}

static esp_err_t conversation_service_append_varint(uint8_t *out, size_t out_len,
                                                    uint64_t value, size_t *used)
{
    while (value >= 0x80U) {
        if (*used >= out_len) {
            return ESP_ERR_NO_MEM;
        }
        out[(*used)++] = (uint8_t)((value & 0x7fU) | 0x80U);
        value >>= 7U;
    }

    if (*used >= out_len) {
        return ESP_ERR_NO_MEM;
    }

    out[(*used)++] = (uint8_t)value;
    return ESP_OK;
}

static esp_err_t conversation_service_append_tag(uint8_t *out, size_t out_len,
                                                 uint32_t field_number, uint8_t wire_type,
                                                 size_t *used)
{
    uint64_t tag = (((uint64_t)field_number) << 3U) | (uint64_t)wire_type;
    return conversation_service_append_varint(out, out_len, tag, used);
}

static esp_err_t conversation_service_append_bytes_field(uint8_t *out, size_t out_len,
                                                         uint32_t field_number,
                                                         const uint8_t *data, size_t len,
                                                         size_t *used)
{
    esp_err_t ret = conversation_service_append_tag(out, out_len, field_number, 2U, used);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = conversation_service_append_varint(out, out_len, len, used);
    if (ret != ESP_OK) {
        return ret;
    }

    if ((*used + len) > out_len) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(out + *used, data, len);
    *used += len;
    return ESP_OK;
}

static esp_err_t conversation_service_append_string_field(uint8_t *out, size_t out_len,
                                                          uint32_t field_number,
                                                          const char *text, size_t *used)
{
    return conversation_service_append_bytes_field(out, out_len, field_number,
                                                   (const uint8_t *)text, strlen(text), used);
}

static esp_err_t conversation_service_append_varint_field(uint8_t *out, size_t out_len,
                                                          uint32_t field_number, uint64_t value,
                                                          size_t *used)
{
    esp_err_t ret = conversation_service_append_tag(out, out_len, field_number, 0U, used);
    if (ret != ESP_OK) {
        return ret;
    }

    return conversation_service_append_varint(out, out_len, value, used);
}

static esp_err_t conversation_service_encode_audio_format(uint8_t *out, size_t out_len, size_t *used)
{
    *used = 0U;
    esp_err_t ret;

    ret = conversation_service_append_varint_field(out, out_len, 1U, s_conversation.sample_rate, used);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = conversation_service_append_varint_field(out, out_len, 2U, s_conversation.channels, used);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = conversation_service_append_varint_field(out, out_len, 3U, s_conversation.bit_depth, used);
    if (ret != ESP_OK) {
        return ret;
    }

    return conversation_service_append_string_field(out, out_len, 4U, s_conversation.encoding, used);
}

static esp_err_t conversation_service_encode_timestamp(uint8_t *out, size_t out_len, size_t *used)
{
    const int64_t now_us = esp_timer_get_time();
    const uint64_t seconds = (uint64_t)(now_us / 1000000LL);
    const uint64_t nanos = (uint64_t)((now_us % 1000000LL) * 1000LL);
    esp_err_t ret;

    *used = 0U;
    ret = conversation_service_append_varint_field(out, out_len, 1U, seconds, used);
    if (ret != ESP_OK) {
        return ret;
    }

    return conversation_service_append_varint_field(out, out_len, 2U, nanos, used);
}

static esp_err_t conversation_service_encode_audio_chunk(const conversation_audio_item_t *item,
                                                         uint8_t *out, size_t out_len,
                                                         size_t *used)
{
    uint8_t format_msg[64];
    uint8_t ts_msg[32];
    size_t format_len = 0U;
    size_t ts_len = 0U;
    esp_err_t ret;

    *used = 0U;

    ret = conversation_service_encode_audio_format(format_msg, sizeof(format_msg), &format_len);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = conversation_service_encode_timestamp(ts_msg, sizeof(ts_msg), &ts_len);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = conversation_service_append_bytes_field(out, out_len, 1U, item->data, item->len, used);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = conversation_service_append_bytes_field(out, out_len, 2U, format_msg, format_len, used);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = conversation_service_append_varint_field(out, out_len, 3U, item->seq, used);
    if (ret != ESP_OK) {
        return ret;
    }

    return conversation_service_append_bytes_field(out, out_len, 4U, ts_msg, ts_len, used);
}

static bool protobuf_read_varint(const uint8_t *data, size_t len, size_t *offset, uint64_t *value)
{
    uint64_t result = 0U;
    uint8_t shift = 0U;

    while (*offset < len && shift < 64U) {
        uint8_t byte = data[(*offset)++];
        result |= ((uint64_t)(byte & 0x7fU) << shift);
        if ((byte & 0x80U) == 0U) {
            *value = result;
            return true;
        }
        shift = (uint8_t)(shift + 7U);
    }

    return false;
}

static bool protobuf_skip_field(const uint8_t *data, size_t len, size_t *offset, uint8_t wire_type)
{
    uint64_t field_len;

    switch (wire_type) {
    case 0U:
        return protobuf_read_varint(data, len, offset, &field_len);

    case 2U:
        if (!protobuf_read_varint(data, len, offset, &field_len)) {
            return false;
        }
        if ((*offset + field_len) > len) {
            return false;
        }
        *offset += (size_t)field_len;
        return true;

    default:
        return false;
    }
}

static bool conversation_service_parse_audio_output(const uint8_t *data, size_t len,
                                                    size_t *audio_len, uint64_t *seq)
{
    size_t offset = 0U;
    *audio_len = 0U;
    *seq = 0U;

    while (offset < len) {
        uint64_t key;
        if (!protobuf_read_varint(data, len, &offset, &key)) {
            return false;
        }

        const uint32_t field_number = (uint32_t)(key >> 3U);
        const uint8_t wire_type = (uint8_t)(key & 0x07U);

        if (field_number == 1U && wire_type == 2U) {
            uint64_t bytes_len;
            if (!protobuf_read_varint(data, len, &offset, &bytes_len) ||
                (offset + bytes_len) > len) {
                return false;
            }
            *audio_len = (size_t)bytes_len;
            offset += (size_t)bytes_len;
        } else if (field_number == 2U && wire_type == 0U) {
            if (!protobuf_read_varint(data, len, &offset, seq)) {
                return false;
            }
        } else if (!protobuf_skip_field(data, len, &offset, wire_type)) {
            return false;
        }
    }

    return true;
}

static bool conversation_service_parse_audio_complete(const uint8_t *data, size_t len, uint64_t *chunks)
{
    size_t offset = 0U;
    *chunks = 0U;

    while (offset < len) {
        uint64_t key;
        if (!protobuf_read_varint(data, len, &offset, &key)) {
            return false;
        }

        const uint32_t field_number = (uint32_t)(key >> 3U);
        const uint8_t wire_type = (uint8_t)(key & 0x07U);

        if (field_number == 1U && wire_type == 0U) {
            if (!protobuf_read_varint(data, len, &offset, chunks)) {
                return false;
            }
        } else if (!protobuf_skip_field(data, len, &offset, wire_type)) {
            return false;
        }
    }

    return true;
}

static bool conversation_service_parse_error_event(const uint8_t *data, size_t len,
                                                   char *code, size_t code_len,
                                                   char *message, size_t message_len)
{
    size_t offset = 0U;
    code[0] = '\0';
    message[0] = '\0';

    while (offset < len) {
        uint64_t key;
        if (!protobuf_read_varint(data, len, &offset, &key)) {
            return false;
        }

        const uint32_t field_number = (uint32_t)(key >> 3U);
        const uint8_t wire_type = (uint8_t)(key & 0x07U);

        if ((field_number == 1U || field_number == 2U) && wire_type == 2U) {
            uint64_t field_len;
            if (!protobuf_read_varint(data, len, &offset, &field_len) ||
                (offset + field_len) > len) {
                return false;
            }

            char *dst = (field_number == 1U) ? code : message;
            size_t dst_len = (field_number == 1U) ? code_len : message_len;
            size_t copy_len = (size_t)field_len < (dst_len - 1U) ? (size_t)field_len : (dst_len - 1U);
            memcpy(dst, data + offset, copy_len);
            dst[copy_len] = '\0';
            offset += (size_t)field_len;
        } else if (!protobuf_skip_field(data, len, &offset, wire_type)) {
            return false;
        }
    }

    return true;
}

static bool conversation_service_parse_conversation_event(const uint8_t *data, size_t len)
{
    size_t offset = 0U;

    while (offset < len) {
        uint64_t key;
        if (!protobuf_read_varint(data, len, &offset, &key)) {
            return false;
        }

        const uint32_t field_number = (uint32_t)(key >> 3U);
        const uint8_t wire_type = (uint8_t)(key & 0x07U);

        if (wire_type != 2U) {
            return protobuf_skip_field(data, len, &offset, wire_type);
        }

        uint64_t msg_len;
        if (!protobuf_read_varint(data, len, &offset, &msg_len) ||
            (offset + msg_len) > len) {
            return false;
        }

        const uint8_t *msg = data + offset;
        const size_t nested_len = (size_t)msg_len;

        if (field_number == 1U) {
            size_t audio_len = 0U;
            uint64_t seq = 0U;
            if (!conversation_service_parse_audio_output(msg, nested_len, &audio_len, &seq)) {
                return false;
            }
            s_conversation.rx_audio_chunks++;
            s_conversation.rx_audio_bytes += (uint32_t)audio_len;
            snprintf(s_conversation.last_event, sizeof(s_conversation.last_event),
                     "audio_output seq=%llu bytes=%u",
                     (unsigned long long)seq, (unsigned int)audio_len);
            ESP_LOGI(TAG, "gRPC RX audio_output seq=%llu bytes=%u",
                     (unsigned long long)seq, (unsigned int)audio_len);
        } else if (field_number == 2U) {
            uint64_t chunks = 0U;
            if (!conversation_service_parse_audio_complete(msg, nested_len, &chunks)) {
                return false;
            }
            s_conversation.rx_audio_complete_count++;
            snprintf(s_conversation.last_event, sizeof(s_conversation.last_event),
                     "audio_complete chunks=%llu", (unsigned long long)chunks);
            ESP_LOGI(TAG, "gRPC RX audio_complete chunks=%llu", (unsigned long long)chunks);
        } else if (field_number == 100U) {
            char code[32];
            char message[64];
            if (!conversation_service_parse_error_event(msg, nested_len,
                                                        code, sizeof(code),
                                                        message, sizeof(message))) {
                return false;
            }
            s_conversation.rx_error_count++;
            snprintf(s_conversation.last_event, sizeof(s_conversation.last_event),
                     "error=%.24s %.48s",
                     code[0] != '\0' ? code : "-",
                     message[0] != '\0' ? message : "-");
            ESP_LOGW(TAG, "gRPC RX error=%s %s",
                     code[0] != '\0' ? code : "-", message[0] != '\0' ? message : "-");
        }

        offset += nested_len;
    }

    return true;
}

static esp_err_t conversation_service_process_grpc_data(const uint8_t *payload, size_t payload_len)
{
    size_t offset = 0U;

    while ((offset + CONVERSATION_GRPC_PREFIX_LEN) <= payload_len) {
        const uint8_t compressed = payload[offset];
        const uint32_t msg_len =
            ((uint32_t)payload[offset + 1] << 24) |
            ((uint32_t)payload[offset + 2] << 16) |
            ((uint32_t)payload[offset + 3] << 8) |
            ((uint32_t)payload[offset + 4]);

        offset += CONVERSATION_GRPC_PREFIX_LEN;
        if ((offset + msg_len) > payload_len) {
            conversation_service_set_error("grpc_frame_truncated");
            return ESP_FAIL;
        }

        if (compressed != 0U) {
            conversation_service_set_error("grpc_compression_unsupported");
            return ESP_FAIL;
        }

        if (!conversation_service_parse_conversation_event(payload + offset, msg_len)) {
            conversation_service_set_error("grpc_event_parse_failed");
            return ESP_FAIL;
        }

        offset += msg_len;
    }

    return ESP_OK;
}

static esp_err_t conversation_service_send_stream_headers(void)
{
    uint8_t block[CONVERSATION_HEADERS_BLOCK_MAX_LEN];
    char authority[CONVERSATION_HOST_MAX_LEN + 8];
    size_t used = 0U;
    size_t chunk;

    snprintf(authority, sizeof(authority), "%s:%u", s_conversation.host, (unsigned int)s_conversation.port);

    chunk = hpack_append_indexed(block + used, sizeof(block) - used, 3U);
    if (chunk == 0U) return ESP_ERR_NO_MEM;
    used += chunk;

    chunk = hpack_append_indexed(block + used, sizeof(block) - used, 6U);
    if (chunk == 0U) return ESP_ERR_NO_MEM;
    used += chunk;

    chunk = hpack_append_literal_indexed_name(block + used, sizeof(block) - used, 4U, GRPC_STREAM_PATH);
    if (chunk == 0U) return ESP_ERR_NO_MEM;
    used += chunk;

    chunk = hpack_append_literal_indexed_name(block + used, sizeof(block) - used, 1U, authority);
    if (chunk == 0U) return ESP_ERR_NO_MEM;
    used += chunk;

    chunk = hpack_append_literal_indexed_name(block + used, sizeof(block) - used, 31U, GRPC_CONTENT_TYPE);
    if (chunk == 0U) return ESP_ERR_NO_MEM;
    used += chunk;

    chunk = hpack_append_literal_indexed_name(block + used, sizeof(block) - used, 54U, GRPC_TE_VALUE);
    if (chunk == 0U) return ESP_ERR_NO_MEM;
    used += chunk;

    chunk = hpack_append_literal_indexed_name(block + used, sizeof(block) - used, 58U, GRPC_USER_AGENT);
    if (chunk == 0U) return ESP_ERR_NO_MEM;
    used += chunk;

    chunk = hpack_append_literal_new_name(block + used, sizeof(block) - used, "user-id", s_conversation.user_id);
    if (chunk == 0U) return ESP_ERR_NO_MEM;
    used += chunk;

    if (s_conversation.session_id[0] != '\0') {
        chunk = hpack_append_literal_new_name(block + used, sizeof(block) - used,
                                              "session-id", s_conversation.session_id);
        if (chunk == 0U) return ESP_ERR_NO_MEM;
        used += chunk;
    }

    return conversation_service_send_frame(HTTP2_FRAME_HEADERS,
                                           HTTP2_FLAG_END_HEADERS,
                                           s_conversation.active_stream_id,
                                           block,
                                           (uint32_t)used);
}

static void conversation_service_generate_default_session_id(void)
{
    if (s_conversation.session_id[0] != '\0') {
        return;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000LL;
    snprintf(s_conversation.session_id, sizeof(s_conversation.session_id),
             "%s-%lld", CONVERSATION_DEFAULT_SESSION_PREFIX, (long long)now_ms);
}

static esp_err_t conversation_service_open_stream_if_needed(void)
{
    if (s_conversation.stream_state == CONVERSATION_STREAM_OPEN ||
        s_conversation.stream_state == CONVERSATION_STREAM_OPENING) {
        return ESP_OK;
    }

    if (s_conversation.transport_state != CONVERSATION_TRANSPORT_READY ||
        s_conversation.sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    conversation_service_generate_default_session_id();
    s_conversation.active_stream_id = s_conversation.next_stream_id;
    s_conversation.next_stream_id += 2U;
    s_conversation.stream_state = CONVERSATION_STREAM_OPENING;
    s_conversation.response_headers_seen = false;
    s_conversation.response_trailers_seen = false;

    esp_err_t ret = conversation_service_send_stream_headers();
    if (ret != ESP_OK) {
        s_conversation.stream_state = CONVERSATION_STREAM_FAILED;
        s_conversation.stream_failure_count++;
        conversation_service_set_error("send_headers_failed");
        return ret;
    }

    s_conversation.stream_open_count++;
    s_conversation.auto_session_started = true;
    conversation_service_set_event("stream_headers_sent");
    ESP_LOGI(TAG, "gRPC stream opening: stream=%u session=%s",
             (unsigned int)s_conversation.active_stream_id,
             s_conversation.session_id);
    return ESP_OK;
}

static esp_err_t conversation_service_send_audio_frame(const conversation_audio_item_t *item)
{
    uint8_t grpc_payload[CONVERSATION_GRPC_PREFIX_LEN + CONVERSATION_AUDIO_MAX_BYTES + 128];
    size_t message_len = 0U;
    esp_err_t ret = conversation_service_encode_audio_chunk(
        item,
        grpc_payload + CONVERSATION_GRPC_PREFIX_LEN,
        sizeof(grpc_payload) - CONVERSATION_GRPC_PREFIX_LEN,
        &message_len);
    if (ret != ESP_OK) {
        return ret;
    }

    grpc_payload[0] = 0x00;
    grpc_payload[1] = (uint8_t)((message_len >> 24) & 0xffU);
    grpc_payload[2] = (uint8_t)((message_len >> 16) & 0xffU);
    grpc_payload[3] = (uint8_t)((message_len >> 8) & 0xffU);
    grpc_payload[4] = (uint8_t)(message_len & 0xffU);

    ret = conversation_service_send_frame(HTTP2_FRAME_DATA,
                                          0x00,
                                          s_conversation.active_stream_id,
                                          grpc_payload,
                                          (uint32_t)(CONVERSATION_GRPC_PREFIX_LEN + message_len));
    if (ret == ESP_OK) {
        s_conversation.tx_audio_chunks++;
        s_conversation.tx_audio_bytes += item->len;
        snprintf(s_conversation.last_event, sizeof(s_conversation.last_event),
                 "tx_audio seq=%llu bytes=%u",
                 (unsigned long long)item->seq, (unsigned int)item->len);
    }

    return ret;
}

static void conversation_service_process_send_queue(void)
{
    conversation_audio_item_t item;

    if (s_conversation.transport_state != CONVERSATION_TRANSPORT_READY ||
        s_conversation.sock < 0 ||
        s_conversation.active_stream_id == 0U ||
        (s_conversation.stream_state != CONVERSATION_STREAM_OPEN &&
         s_conversation.stream_state != CONVERSATION_STREAM_OPENING)) {
        return;
    }

    while (xQueueReceive(s_audio_queue, &item, 0) == pdTRUE) {
        esp_err_t ret = conversation_service_send_audio_frame(&item);
        if (ret != ESP_OK) {
            conversation_service_set_error("send_audio_failed");
            s_conversation.stream_state = CONVERSATION_STREAM_FAILED;
            s_conversation.stream_failure_count++;
            conversation_service_close_socket();
            ESP_LOGW(TAG, "gRPC TX failed: seq=%llu bytes=%u",
                     (unsigned long long)item.seq, (unsigned int)item.len);
            break;
        }

        ESP_LOGI(TAG, "gRPC TX audio seq=%llu bytes=%u",
                 (unsigned long long)item.seq, (unsigned int)item.len);
    }
}

static void conversation_service_handle_headers_frame(uint32_t stream_id, uint8_t flags)
{
    if (stream_id != s_conversation.active_stream_id) {
        return;
    }

    if (!s_conversation.response_headers_seen) {
        s_conversation.response_headers_seen = true;
        s_conversation.stream_state = CONVERSATION_STREAM_OPEN;
        conversation_service_set_event("stream_response_headers");
        ESP_LOGI(TAG, "gRPC stream open: stream=%u session=%s",
                 (unsigned int)stream_id, s_conversation.session_id);
    }

    if ((flags & HTTP2_FLAG_END_STREAM) != 0U) {
        s_conversation.response_trailers_seen = true;
        s_conversation.stream_state = CONVERSATION_STREAM_CLOSED;
        conversation_service_set_event("stream_remote_closed");
        ESP_LOGI(TAG, "gRPC stream closed by server: stream=%u", (unsigned int)stream_id);
    }
}

static void conversation_service_handle_frame(uint8_t frame_type, uint8_t flags, uint32_t stream_id,
                                              const uint8_t *payload, uint32_t payload_len)
{
    switch (frame_type) {
    case HTTP2_FRAME_SETTINGS:
        s_conversation.server_settings_seen = true;
        if ((flags & HTTP2_SETTINGS_ACK_FLAG) == 0U) {
            (void)conversation_service_send_settings_ack(s_conversation.sock);
        }
        break;

    case HTTP2_FRAME_HEADERS:
        conversation_service_handle_headers_frame(stream_id, flags);
        break;

    case HTTP2_FRAME_DATA:
        if (stream_id == s_conversation.active_stream_id) {
            if (conversation_service_process_grpc_data(payload, payload_len) != ESP_OK) {
                s_conversation.stream_state = CONVERSATION_STREAM_FAILED;
                s_conversation.stream_failure_count++;
                ESP_LOGW(TAG, "gRPC RX data parse failed: %s",
                         s_conversation.last_error[0] != '\0' ? s_conversation.last_error : "unknown");
            }

            if ((flags & HTTP2_FLAG_END_STREAM) != 0U) {
                s_conversation.stream_state = CONVERSATION_STREAM_CLOSED;
                conversation_service_set_event("stream_data_end");
            }
        }
        break;

    case HTTP2_FRAME_RST_STREAM:
        if (stream_id == s_conversation.active_stream_id) {
            s_conversation.stream_state = CONVERSATION_STREAM_FAILED;
            s_conversation.stream_failure_count++;
            conversation_service_set_error("rst_stream");
            ESP_LOGW(TAG, "gRPC stream reset by server");
        }
        break;

    case HTTP2_FRAME_GOAWAY:
        s_conversation.transport_state = CONVERSATION_TRANSPORT_FAILED;
        s_conversation.transport_failure_count++;
        conversation_service_set_error("goaway");
        conversation_service_close_socket();
        ESP_LOGW(TAG, "HTTP/2 GOAWAY received");
        break;

    default:
        break;
    }
}

static void conversation_service_poll_frames(void)
{
    if (s_conversation.transport_state != CONVERSATION_TRANSPORT_READY || s_conversation.sock < 0) {
        return;
    }

    for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s_conversation.sock, &readfds);
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = 0,
        };

        int sel_ret = select(s_conversation.sock + 1, &readfds, NULL, NULL, &timeout);
        if (sel_ret <= 0) {
            break;
        }

        uint8_t header[CONVERSATION_H2_FRAME_HEADER_LEN];
        esp_err_t ret = conversation_service_recv_all_timeout(
            s_conversation.sock, header, sizeof(header),
            CONFIG_COLLAR_CONVERSATION_TRANSPORT_HANDSHAKE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            s_conversation.transport_state = CONVERSATION_TRANSPORT_FAILED;
            s_conversation.transport_failure_count++;
            conversation_service_set_error("recv_frame_header_failed");
            conversation_service_close_socket();
            break;
        }

        const uint32_t payload_len =
            ((uint32_t)header[0] << 16) |
            ((uint32_t)header[1] << 8) |
            ((uint32_t)header[2]);
        const uint8_t frame_type = header[3];
        const uint8_t frame_flags = header[4];
        const uint32_t stream_id =
            (((uint32_t)header[5] & 0x7fU) << 24) |
            ((uint32_t)header[6] << 16) |
            ((uint32_t)header[7] << 8) |
            ((uint32_t)header[8]);

        uint8_t *payload = NULL;
        if (payload_len > 0U) {
            if (payload_len > CONVERSATION_MAX_FRAME_PAYLOAD_LEN) {
                s_conversation.transport_state = CONVERSATION_TRANSPORT_FAILED;
                s_conversation.transport_failure_count++;
                conversation_service_set_error("frame_too_large");
                conversation_service_close_socket();
                break;
            }

            payload = (uint8_t *)malloc(payload_len);
            if (payload == NULL) {
                s_conversation.transport_state = CONVERSATION_TRANSPORT_FAILED;
                s_conversation.transport_failure_count++;
                conversation_service_set_error("malloc_frame_failed");
                conversation_service_close_socket();
                break;
            }

            ret = conversation_service_recv_all_timeout(
                s_conversation.sock, payload, payload_len,
                CONFIG_COLLAR_CONVERSATION_TRANSPORT_HANDSHAKE_TIMEOUT_MS);
            if (ret != ESP_OK) {
                free(payload);
                s_conversation.transport_state = CONVERSATION_TRANSPORT_FAILED;
                s_conversation.transport_failure_count++;
                conversation_service_set_error("recv_frame_payload_failed");
                conversation_service_close_socket();
                break;
            }
        }

        conversation_service_handle_frame(frame_type, frame_flags, stream_id, payload, payload_len);
        free(payload);

        if (s_conversation.transport_state != CONVERSATION_TRANSPORT_READY) {
            break;
        }
    }
}

static void conversation_service_transport_tick(void)
{
    if (!CONFIG_COLLAR_CONVERSATION_TRANSPORT_ENABLE) {
        return;
    }

    if (!s_conversation.configured ||
        !s_conversation.wifi_ready ||
        !cloud_service_is_reachable() ||
        strcmp(cloud_service_host(), s_conversation.host) != 0 ||
        cloud_service_port() != s_conversation.port) {
        if (s_conversation.transport_state == CONVERSATION_TRANSPORT_READY) {
            ESP_LOGI(TAG, "Conversation transport paused: prerequisites not satisfied");
        }
        conversation_service_reset_transport_state(false);
        return;
    }

    if (s_conversation.transport_state != CONVERSATION_TRANSPORT_READY) {
        if (!conversation_service_transport_due(esp_timer_get_time())) {
            return;
        }

        esp_err_t ret = conversation_service_open_transport();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "Conversation transport ready: peer=%s frame=%s flags=0x%02x settings=%s",
                     s_conversation.peer_addr[0] != '\0' ? s_conversation.peer_addr : "-",
                     conversation_frame_type_string(s_conversation.first_frame_type),
                     (unsigned int)s_conversation.first_frame_flags,
                     s_conversation.server_settings_seen ? "yes" : "no");
        } else {
            ESP_LOGW(TAG, "Conversation transport failed: error=%s",
                     s_conversation.last_error[0] != '\0' ? s_conversation.last_error : "unknown");
        }
        return;
    }

    if (!s_conversation.auto_session_started) {
        (void)conversation_service_open_stream_if_needed();
    }

    conversation_service_poll_frames();
    conversation_service_process_send_queue();
}

static void conversation_service_task(void *arg)
{
    (void)arg;

    for (;;) {
        conversation_service_transport_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t conversation_service_start(void)
{
    if (s_conversation.started) {
        return ESP_OK;
    }

    memset(&s_conversation, 0, sizeof(s_conversation));
    s_conversation.sock = -1;
    s_conversation.first_frame_type = 0xff;
    s_conversation.next_stream_id = 1U;
    strlcpy(s_conversation.host, CONFIG_COLLAR_CONVERSATION_HOST, sizeof(s_conversation.host));
    strlcpy(s_conversation.user_id, CONFIG_COLLAR_CONVERSATION_USER_ID, sizeof(s_conversation.user_id));
    strlcpy(s_conversation.encoding, CONFIG_COLLAR_CONVERSATION_ENCODING, sizeof(s_conversation.encoding));
    s_conversation.port = (uint16_t)CONFIG_COLLAR_CONVERSATION_PORT;
    s_conversation.sample_rate = (uint32_t)CONFIG_COLLAR_CONVERSATION_AUDIO_SAMPLE_RATE;
    s_conversation.channels = (uint8_t)CONFIG_COLLAR_CONVERSATION_AUDIO_CHANNELS;
    s_conversation.bit_depth = (uint8_t)CONFIG_COLLAR_CONVERSATION_AUDIO_BIT_DEPTH;
    s_conversation.configured = CONFIG_COLLAR_CONVERSATION_ENABLE && s_conversation.host[0] != '\0';
    conversation_service_reset_stream_state(true);

    s_audio_queue = xQueueCreateStatic(
        CONVERSATION_AUDIO_QUEUE_LEN,
        sizeof(conversation_audio_item_t),
        s_audio_queue_storage,
        &s_audio_queue_tcb);
    if (s_audio_queue == NULL) {
        return ESP_FAIL;
    }

    s_audio_enqueue_mutex = xSemaphoreCreateMutexStatic(&s_audio_enqueue_mutex_tcb);
    if (s_audio_enqueue_mutex == NULL) {
        return ESP_FAIL;
    }

    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        conversation_service_task,
        "conv_svc",
        CONVERSATION_SERVICE_STACK_WORDS,
        NULL,
        CONVERSATION_SERVICE_PRIORITY,
        s_conversation_stack,
        &s_conversation_tcb,
        tskNO_AFFINITY);
    if (task_handle == NULL) {
        return ESP_FAIL;
    }

    s_conversation.started = true;

    if (!CONFIG_COLLAR_CONVERSATION_ENABLE) {
        ESP_LOGI(TAG, "Conversation service disabled by config");
    } else if (!s_conversation.configured) {
        ESP_LOGI(TAG, "Conversation service idle: host not configured");
    } else {
        ESP_LOGI(TAG,
                 "Conversation target armed: host=%s port=%u user=%s audio=%lu/%u/%u enc=%s up_b64=%s down_b64=%s auto_b64=%s transport=%s",
                 s_conversation.host,
                 (unsigned int)s_conversation.port,
                 s_conversation.user_id,
                 (unsigned long)s_conversation.sample_rate,
                 (unsigned int)s_conversation.channels,
                 (unsigned int)s_conversation.bit_depth,
                 s_conversation.encoding,
                 conversation_uplink_base64_string(),
                 conversation_downlink_base64_string(),
                 conversation_autodetect_base64_string(),
                 CONFIG_COLLAR_CONVERSATION_TRANSPORT_ENABLE ? "on" : "off");
    }

    return ESP_OK;
}

void conversation_service_handle_wifi_state(kernel_wifi_state_t state)
{
    switch (state) {
    case KERNEL_WIFI_STATE_GOT_IP:
        s_conversation.wifi_ready = true;
        break;

    case KERNEL_WIFI_STATE_PROVISIONING:
    case KERNEL_WIFI_STATE_CONNECTING:
    case KERNEL_WIFI_STATE_DISCONNECTED:
    case KERNEL_WIFI_STATE_FAILED:
        s_conversation.wifi_ready = false;
        conversation_service_reset_transport_state(false);
        break;

    default:
        break;
    }
}

void conversation_service_log_status(void)
{
    const char *probe_error = cloud_service_last_error();

    ESP_LOGI(TAG,
             "Conversation status: state=%s host=%s port=%u user=%s session=%s wifi=%s cloud=%s probe_err=%s transport_err=%s peer=%s frame=%s tx=%lu/%lu rx=%lu/%lu complete=%lu err=%lu last=%s audio=%lu/%u/%u enc=%s up_b64=%s down_b64=%s auto_b64=%s",
             conversation_service_state_string(),
             s_conversation.host[0] != '\0' ? s_conversation.host : "-",
             (unsigned int)s_conversation.port,
             s_conversation.user_id[0] != '\0' ? s_conversation.user_id : "-",
             s_conversation.session_id[0] != '\0' ? s_conversation.session_id : "-",
             s_conversation.wifi_ready ? "ready" : "no",
             cloud_service_is_reachable() ? "reachable" : "no",
             probe_error[0] != '\0' ? probe_error : "-",
             s_conversation.last_error[0] != '\0' ? s_conversation.last_error : "-",
             s_conversation.peer_addr[0] != '\0' ? s_conversation.peer_addr : "-",
             s_conversation.first_frame_type == 0xff ?
                 "-" : conversation_frame_type_string(s_conversation.first_frame_type),
             (unsigned long)s_conversation.tx_audio_chunks,
             (unsigned long)s_conversation.tx_audio_bytes,
             (unsigned long)s_conversation.rx_audio_chunks,
             (unsigned long)s_conversation.rx_audio_bytes,
             (unsigned long)s_conversation.rx_audio_complete_count,
             (unsigned long)s_conversation.rx_error_count,
             s_conversation.last_event[0] != '\0' ? s_conversation.last_event : "-",
             (unsigned long)s_conversation.sample_rate,
             (unsigned int)s_conversation.channels,
             (unsigned int)s_conversation.bit_depth,
             s_conversation.encoding[0] != '\0' ? s_conversation.encoding : "-",
             conversation_uplink_base64_string(),
             conversation_downlink_base64_string(),
             conversation_autodetect_base64_string());
}

bool conversation_service_is_configured(void)
{
    return s_conversation.configured;
}

const char *conversation_service_host(void)
{
    return s_conversation.host;
}

uint16_t conversation_service_port(void)
{
    return s_conversation.port;
}

bool conversation_service_transport_ready(void)
{
    return s_conversation.transport_state == CONVERSATION_TRANSPORT_READY;
}

bool conversation_service_stream_ready(void)
{
    return s_conversation.stream_state == CONVERSATION_STREAM_OPEN;
}

bool conversation_service_stream_writable(void)
{
    return s_conversation.transport_state == CONVERSATION_TRANSPORT_READY &&
           s_conversation.sock >= 0 &&
           s_conversation.active_stream_id != 0U &&
           (s_conversation.stream_state == CONVERSATION_STREAM_OPEN ||
            s_conversation.stream_state == CONVERSATION_STREAM_OPENING);
}

esp_err_t conversation_service_start_session(const char *session_id)
{
    if (session_id == NULL || session_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_conversation.session_id, session_id, sizeof(s_conversation.session_id));
    s_conversation.auto_session_started = false;
    conversation_service_reset_stream_state(false);
    conversation_service_set_event("session_requested");
    return ESP_OK;
}

esp_err_t conversation_service_send_audio(const uint8_t *pcm, size_t len, uint64_t seq)
{
    if (pcm == NULL || len == 0U || len > CONVERSATION_AUDIO_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_audio_queue == NULL || s_audio_enqueue_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_audio_enqueue_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_audio_enqueue_item.len = (uint16_t)len;
    s_audio_enqueue_item.seq = seq;
    memcpy(s_audio_enqueue_item.data, pcm, len);

    BaseType_t queued = xQueueSend(s_audio_queue, &s_audio_enqueue_item, 0);
    xSemaphoreGive(s_audio_enqueue_mutex);

    if (queued != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
