#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "app/app_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "bsp/microphone_input.h"
#include "bsp/speaker_output.h"
#include "kernel/kernel_msgbus.h"
#include "kernel/kernel_trace.h"
#include "services/conversation_service.h"
#include "services/wifi_service.h"

#define APP_MANAGER_STACK_WORDS           4096
#define APP_MANAGER_PRIORITY              8
#define APP_MANAGER_CORE                  tskNO_AFFINITY
#define APP_HEARTBEAT_PERIOD_MS           1000
#define APP_LOOP_PERIOD_MS                20
#define APP_SELF_TEST_TAIL_SILENCE_CHUNKS 5U
#define APP_SELF_TEST_MAX_CHUNK_BYTES     2048
#define APP_MIC_TEST_MAX_CHUNK_BYTES      4096
#define APP_SPEAKER_TEST_MAX_CHUNK_BYTES  2048
#define APP_SPEAKER_TEST_MAX_LOOPS        5U
#define APP_MIC_STREAM_STATUS_LOG_US      5000000LL

static const char *TAG = "app_mgr";

static StaticTask_t s_app_tcb;
static StackType_t s_app_stack[APP_MANAGER_STACK_WORDS];
static bool s_app_started;

#if CONFIG_COLLAR_CONVERSATION_SELF_TEST_ENABLE
typedef struct {
    bool session_requested;
    bool completion_logged;
    bool ending_requested;
    bool tail_logged;
    bool start_delay_logged;
    bool completed_once;
    uint32_t sent_chunks;
    uint32_t target_chunks;
    uint32_t tail_chunks_sent;
    uint32_t tail_chunks_target;
    uint32_t chunk_bytes;
    uint64_t next_seq;
    int64_t next_send_us;
    int64_t restart_after_us;
    char session_id[48];
} app_self_test_state_t;

static app_self_test_state_t s_self_test;

static size_t app_self_test_chunk_bytes(void)
{
    const uint32_t bytes_per_sample = (CONFIG_COLLAR_CONVERSATION_AUDIO_BIT_DEPTH + 7U) / 8U;
    const uint32_t bytes_per_frame =
        CONFIG_COLLAR_CONVERSATION_AUDIO_CHANNELS * bytes_per_sample;
    const uint32_t bytes_per_second =
        CONFIG_COLLAR_CONVERSATION_AUDIO_SAMPLE_RATE * bytes_per_frame;
    uint32_t chunk_bytes =
        (bytes_per_second * CONFIG_COLLAR_CONVERSATION_SELF_TEST_CHUNK_MS) / 1000U;

    if (bytes_per_frame == 0U) {
        return 0U;
    }

    chunk_bytes = (chunk_bytes / bytes_per_frame) * bytes_per_frame;
    if (chunk_bytes == 0U) {
        chunk_bytes = bytes_per_frame;
    }

    if (chunk_bytes > APP_SELF_TEST_MAX_CHUNK_BYTES) {
        return 0U;
    }

    return (size_t)chunk_bytes;
}

static void app_self_test_fill_pcm(uint8_t *buffer, size_t len, uint64_t seq)
{
    (void)seq;
    memset(buffer, 0, len);
}

static void app_self_test_reset(int64_t now_us, bool keep_delay)
{
    const bool completed_once = s_self_test.completed_once;
    memset(&s_self_test, 0, sizeof(s_self_test));
    s_self_test.completed_once = completed_once;
    s_self_test.chunk_bytes = (uint32_t)app_self_test_chunk_bytes();
    s_self_test.tail_chunks_target = APP_SELF_TEST_TAIL_SILENCE_CHUNKS;
    if (CONFIG_COLLAR_CONVERSATION_SELF_TEST_CHUNK_MS > 0) {
        s_self_test.target_chunks =
            (uint32_t)((CONFIG_COLLAR_CONVERSATION_SELF_TEST_TOTAL_MS +
                        CONFIG_COLLAR_CONVERSATION_SELF_TEST_CHUNK_MS - 1) /
                       CONFIG_COLLAR_CONVERSATION_SELF_TEST_CHUNK_MS);
    }
    if (keep_delay) {
        s_self_test.restart_after_us =
            now_us + ((int64_t)CONFIG_COLLAR_CONVERSATION_SELF_TEST_REPEAT_DELAY_SEC * 1000000LL);
    }
}

static void app_self_test_tick(void)
{
    static uint8_t pcm_buffer[APP_SELF_TEST_MAX_CHUNK_BYTES];

    const int64_t now_us = esp_timer_get_time();

    if (!conversation_service_is_configured()) {
        return;
    }

    if (s_self_test.completed_once) {
        return;
    }

    if (!conversation_service_transport_ready()) {
        if (s_self_test.session_requested || s_self_test.sent_chunks > 0U) {
            ESP_LOGI(TAG, "Conversation self-test paused: transport not ready");
        }
        app_self_test_reset(now_us, false);
        return;
    }

    if (s_self_test.restart_after_us != 0 && now_us < s_self_test.restart_after_us) {
        return;
    }

    if (!s_self_test.session_requested) {
        if (s_self_test.chunk_bytes == 0U || s_self_test.target_chunks == 0U) {
            ESP_LOGE(TAG, "Conversation self-test config invalid: chunk_bytes=%lu target_chunks=%lu",
                     (unsigned long)s_self_test.chunk_bytes,
                     (unsigned long)s_self_test.target_chunks);
            s_self_test.restart_after_us =
                now_us + ((int64_t)CONFIG_COLLAR_CONVERSATION_SELF_TEST_REPEAT_DELAY_SEC * 1000000LL);
            return;
        }

        snprintf(s_self_test.session_id, sizeof(s_self_test.session_id),
                 "selftest-%lld", (long long)(now_us / 1000LL));
        esp_err_t ret = conversation_service_start_session(s_self_test.session_id);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Conversation self-test failed to request session: %s",
                     esp_err_to_name(ret));
            s_self_test.restart_after_us =
                now_us + ((int64_t)CONFIG_COLLAR_CONVERSATION_SELF_TEST_REPEAT_DELAY_SEC * 1000000LL);
            return;
        }

        s_self_test.session_requested = true;
        s_self_test.next_seq = 1U;
        s_self_test.next_send_us =
            now_us + ((int64_t)CONFIG_COLLAR_CONVERSATION_SELF_TEST_START_DELAY_SEC * 1000000LL);
        ESP_LOGI(TAG,
                 "Conversation self-test session requested: id=%s chunk=%luB count=%lu total=%dms start_delay=%ds",
                 s_self_test.session_id,
                 (unsigned long)s_self_test.chunk_bytes,
                 (unsigned long)s_self_test.target_chunks,
                 CONFIG_COLLAR_CONVERSATION_SELF_TEST_TOTAL_MS,
                 CONFIG_COLLAR_CONVERSATION_SELF_TEST_START_DELAY_SEC);
        return;
    }

    if (!conversation_service_stream_writable()) {
        if (s_self_test.ending_requested && !conversation_service_session_active()) {
            ESP_LOGI(TAG, "Conversation self-test session closed: session=%s",
                     s_self_test.session_id);
            s_self_test.completion_logged = true;
            s_self_test.completed_once = true;
            ESP_LOGI(TAG, "Conversation self-test finished; staying idle");
        }
        return;
    }

    if (s_self_test.ending_requested) {
        if (!conversation_service_session_active() && !s_self_test.completion_logged) {
            ESP_LOGI(TAG, "Conversation self-test session closed: session=%s",
                     s_self_test.session_id);
            s_self_test.completion_logged = true;
            s_self_test.completed_once = true;
            ESP_LOGI(TAG, "Conversation self-test finished; staying idle");
        }
        return;
    }

    if (now_us < s_self_test.next_send_us) {
        if (!s_self_test.start_delay_logged &&
            CONFIG_COLLAR_CONVERSATION_SELF_TEST_START_DELAY_SEC > 0) {
            s_self_test.start_delay_logged = true;
            ESP_LOGI(TAG, "Conversation self-test waiting before TX: session=%s delay=%ds",
                     s_self_test.session_id,
                     CONFIG_COLLAR_CONVERSATION_SELF_TEST_START_DELAY_SEC);
        }
        return;
    }

    bool send_tail_silence = s_self_test.sent_chunks >= s_self_test.target_chunks;
    if (send_tail_silence) {
        if (s_self_test.tail_chunks_sent >= s_self_test.tail_chunks_target) {
            esp_err_t ret = conversation_service_end_session();
            if (ret == ESP_OK) {
                s_self_test.ending_requested = true;
                s_self_test.completed_once = true;
                ESP_LOGI(TAG,
                         "Conversation self-test requested session end: session=%s audio_chunks=%lu silence_chunks=%lu",
                         s_self_test.session_id,
                         (unsigned long)s_self_test.sent_chunks,
                         (unsigned long)s_self_test.tail_chunks_sent);
            } else {
                ESP_LOGW(TAG, "Conversation self-test end request failed: %s",
                         esp_err_to_name(ret));
                s_self_test.restart_after_us =
                    now_us + ((int64_t)CONFIG_COLLAR_CONVERSATION_SELF_TEST_REPEAT_DELAY_SEC * 1000000LL);
            }
            return;
        }

        memset(pcm_buffer, 0, s_self_test.chunk_bytes);
        if (!s_self_test.tail_logged) {
            s_self_test.tail_logged = true;
            ESP_LOGI(TAG, "Conversation self-test sending silence tail: session=%s chunks=%lu",
                     s_self_test.session_id,
                     (unsigned long)s_self_test.tail_chunks_target);
        }
    } else {
        app_self_test_fill_pcm(pcm_buffer, s_self_test.chunk_bytes, s_self_test.next_seq);
    }

    esp_err_t ret = conversation_service_send_audio(
        pcm_buffer, s_self_test.chunk_bytes, s_self_test.next_seq);
    if (ret == ESP_OK) {
        if (send_tail_silence) {
            s_self_test.tail_chunks_sent++;
        } else {
            s_self_test.sent_chunks++;
        }
        s_self_test.next_seq++;
        s_self_test.next_send_us =
            now_us + ((int64_t)CONFIG_COLLAR_CONVERSATION_SELF_TEST_CHUNK_MS * 1000LL);

        if (s_self_test.sent_chunks == 1U) {
            ESP_LOGI(TAG, "Conversation self-test started sending audio");
        }
    } else if (ret != ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Conversation self-test send failed: %s", esp_err_to_name(ret));
        s_self_test.restart_after_us =
            now_us + ((int64_t)CONFIG_COLLAR_CONVERSATION_SELF_TEST_REPEAT_DELAY_SEC * 1000000LL);
    }
}
#endif

#if CONFIG_COLLAR_MICROPHONE_TEST_ENABLE
typedef struct {
    bool started;
    uint8_t channels;
    uint32_t chunk_bytes;
    uint32_t frame_bytes;
    uint32_t chunk_frames;
    int64_t next_read_us;
    int64_t stats_window_start_us;
    uint32_t chunk_count;
    uint32_t read_fail_count;
    uint32_t partial_read_count;
    uint32_t misaligned_read_count;
    uint32_t last_bytes_read;
    uint32_t left_peak;
    uint32_t right_peak;
    uint64_t left_abs_sum;
    uint64_t right_abs_sum;
    uint64_t sample_count;
} app_mic_test_state_t;

static app_mic_test_state_t s_mic_test;

#if CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
typedef struct {
    bool initialized;
    bool ready;
    bool waiting_logged;
    int sock;
    struct sockaddr_in dest_addr;
    uint32_t tx_chunks;
    uint32_t tx_bytes;
    uint32_t tx_failures;
    int64_t last_wait_log_us;
    int64_t last_status_log_us;
} app_mic_stream_state_t;

static app_mic_stream_state_t s_mic_stream;

static void app_mic_stream_init(uint32_t sample_rate, uint8_t channels)
{
    if (s_mic_stream.initialized) {
        return;
    }

    if (!wifi_service_sta_ready()) {
        const int64_t now_us = esp_timer_get_time();
        if (!s_mic_stream.waiting_logged ||
            (now_us - s_mic_stream.last_wait_log_us) >= 5000000LL) {
            ESP_LOGI(TAG, "Mic UDP stream waiting for Wi-Fi IP before opening socket");
            s_mic_stream.waiting_logged = true;
            s_mic_stream.last_wait_log_us = now_us;
        }
        return;
    }

    memset(&s_mic_stream, 0, sizeof(s_mic_stream));
    s_mic_stream.initialized = true;
    s_mic_stream.sock = -1;

    s_mic_stream.dest_addr.sin_family = AF_INET;
    s_mic_stream.dest_addr.sin_port = htons(CONFIG_COLLAR_MICROPHONE_STREAM_PORT);
    if (inet_pton(AF_INET, CONFIG_COLLAR_MICROPHONE_STREAM_HOST, &s_mic_stream.dest_addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "Mic UDP stream invalid host: %s", CONFIG_COLLAR_MICROPHONE_STREAM_HOST);
        return;
    }

    s_mic_stream.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_mic_stream.sock < 0) {
        ESP_LOGE(TAG, "Mic UDP stream socket create failed: errno=%d", errno);
        s_mic_stream.sock = -1;
        return;
    }

    s_mic_stream.ready = true;
    ESP_LOGI(TAG,
             "Mic UDP stream ready: src=%s dst=%s:%d format=pcm_s16le rate=%lu channels=%u",
             wifi_service_sta_ip(),
             CONFIG_COLLAR_MICROPHONE_STREAM_HOST,
             CONFIG_COLLAR_MICROPHONE_STREAM_PORT,
             (unsigned long)sample_rate,
             (unsigned int)channels);
}

static void app_mic_stream_send(const uint8_t *data, size_t len)
{
    if (!s_mic_stream.ready || s_mic_stream.sock < 0 || data == NULL || len == 0U) {
        return;
    }

    int sent = sendto(s_mic_stream.sock, data, len, 0,
                      (const struct sockaddr *)&s_mic_stream.dest_addr,
                      sizeof(s_mic_stream.dest_addr));
    if (sent < 0) {
        s_mic_stream.tx_failures++;
        if (s_mic_stream.tx_failures <= 3U || (s_mic_stream.tx_failures % 50U) == 0U) {
            ESP_LOGW(TAG, "Mic UDP send failed: errno=%d fail=%lu",
                     errno, (unsigned long)s_mic_stream.tx_failures);
        }
        return;
    }

    s_mic_stream.tx_chunks++;
    s_mic_stream.tx_bytes += (uint32_t)sent;
}

static void app_mic_stream_log_status_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if ((now_us - s_mic_stream.last_status_log_us) < APP_MIC_STREAM_STATUS_LOG_US) {
        return;
    }

    s_mic_stream.last_status_log_us = now_us;

    if (!wifi_service_sta_ready()) {
        ESP_LOGI(TAG,
                 "Mic UDP stream status: wifi=waiting dst=%s:%d tx=%lu/%lu fail=%lu",
                 CONFIG_COLLAR_MICROPHONE_STREAM_HOST,
                 CONFIG_COLLAR_MICROPHONE_STREAM_PORT,
                 (unsigned long)s_mic_stream.tx_chunks,
                 (unsigned long)s_mic_stream.tx_bytes,
                 (unsigned long)s_mic_stream.tx_failures);
        return;
    }

    if (!s_mic_stream.ready || s_mic_stream.sock < 0) {
        ESP_LOGI(TAG,
                 "Mic UDP stream status: wifi=ready src=%s udp=not-open dst=%s:%d tx=%lu/%lu fail=%lu",
                 wifi_service_sta_ip(),
                 CONFIG_COLLAR_MICROPHONE_STREAM_HOST,
                 CONFIG_COLLAR_MICROPHONE_STREAM_PORT,
                 (unsigned long)s_mic_stream.tx_chunks,
                 (unsigned long)s_mic_stream.tx_bytes,
                 (unsigned long)s_mic_stream.tx_failures);
        return;
    }

    ESP_LOGI(TAG,
             "Mic UDP stream status: wifi=ready src=%s udp=ready dst=%s:%d tx=%lu/%lu fail=%lu",
             wifi_service_sta_ip(),
             CONFIG_COLLAR_MICROPHONE_STREAM_HOST,
             CONFIG_COLLAR_MICROPHONE_STREAM_PORT,
             (unsigned long)s_mic_stream.tx_chunks,
             (unsigned long)s_mic_stream.tx_bytes,
             (unsigned long)s_mic_stream.tx_failures);
}
#endif

static size_t app_mic_test_chunk_bytes(void)
{
    const uint32_t sample_rate = bsp_microphone_get_sample_rate_hz();
    const uint32_t frame_bytes = bsp_microphone_frame_bytes();
    uint32_t chunk_bytes =
        (sample_rate * frame_bytes * CONFIG_COLLAR_MICROPHONE_TEST_CHUNK_MS) / 1000U;

    if (frame_bytes == 0U) {
        return 0U;
    }

    chunk_bytes = (chunk_bytes / frame_bytes) * frame_bytes;
    if (chunk_bytes == 0U) {
        chunk_bytes = frame_bytes;
    }

    if (chunk_bytes > APP_MIC_TEST_MAX_CHUNK_BYTES) {
        return 0U;
    }

    return (size_t)chunk_bytes;
}

static void app_mic_test_accumulate(const int16_t *samples, size_t frame_count, uint8_t channels)
{
    for (size_t frame = 0; frame < frame_count; ++frame) {
        const int32_t left = samples[frame * channels];
        const uint32_t left_abs = (uint32_t)(left < 0 ? -left : left);
        s_mic_test.left_abs_sum += left_abs;
        if (left_abs > s_mic_test.left_peak) {
            s_mic_test.left_peak = left_abs;
        }

        int32_t right = left;
        if (channels > 1U) {
            right = samples[(frame * channels) + 1U];
        }
        const uint32_t right_abs = (uint32_t)(right < 0 ? -right : right);
        s_mic_test.right_abs_sum += right_abs;
        if (right_abs > s_mic_test.right_peak) {
            s_mic_test.right_peak = right_abs;
        }
    }

    s_mic_test.sample_count += frame_count;
}

static void app_mic_test_log_stats(int64_t now_us)
{
    if (s_mic_test.sample_count == 0U) {
#if CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
        ESP_LOGI(TAG,
                 "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu last_bytes=%lu udp=%lu/%lu fail=%lu no samples",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.last_bytes_read,
                 (unsigned long)s_mic_stream.tx_chunks,
                 (unsigned long)s_mic_stream.tx_bytes,
                 (unsigned long)s_mic_stream.tx_failures);
#else
        ESP_LOGI(TAG, "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu last_bytes=%lu no samples",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.last_bytes_read);
#endif
    } else {
        const uint32_t left_avg_abs =
            (uint32_t)(s_mic_test.left_abs_sum / s_mic_test.sample_count);
        const uint32_t right_avg_abs =
            (uint32_t)(s_mic_test.right_abs_sum / s_mic_test.sample_count);
#if CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
        ESP_LOGI(TAG,
                 "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu last_bytes=%lu udp=%lu/%lu fail=%lu frames=%llu L(avg_abs=%lu peak=%lu) R(avg_abs=%lu peak=%lu)",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.last_bytes_read,
                 (unsigned long)s_mic_stream.tx_chunks,
                 (unsigned long)s_mic_stream.tx_bytes,
                 (unsigned long)s_mic_stream.tx_failures,
                 (unsigned long long)s_mic_test.sample_count,
                 (unsigned long)left_avg_abs,
                 (unsigned long)s_mic_test.left_peak,
                 (unsigned long)right_avg_abs,
                 (unsigned long)s_mic_test.right_peak);
#else
        ESP_LOGI(TAG,
                 "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu last_bytes=%lu frames=%llu L(avg_abs=%lu peak=%lu) R(avg_abs=%lu peak=%lu)",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.last_bytes_read,
                 (unsigned long long)s_mic_test.sample_count,
                 (unsigned long)left_avg_abs,
                 (unsigned long)s_mic_test.left_peak,
                 (unsigned long)right_avg_abs,
                 (unsigned long)s_mic_test.right_peak);
#endif
    }

    s_mic_test.chunk_count = 0U;
    s_mic_test.read_fail_count = 0U;
    s_mic_test.partial_read_count = 0U;
    s_mic_test.misaligned_read_count = 0U;
    s_mic_test.last_bytes_read = 0U;
    s_mic_test.left_peak = 0U;
    s_mic_test.right_peak = 0U;
    s_mic_test.left_abs_sum = 0U;
    s_mic_test.right_abs_sum = 0U;
    s_mic_test.sample_count = 0U;
    s_mic_test.stats_window_start_us = now_us;
}

static void app_mic_test_tick(void)
{
    static uint8_t pcm_buffer[APP_MIC_TEST_MAX_CHUNK_BYTES];
    const int64_t chunk_interval_us =
        (int64_t)CONFIG_COLLAR_MICROPHONE_TEST_CHUNK_MS * 1000LL;
    const int64_t stats_interval_us =
        (int64_t)CONFIG_COLLAR_MICROPHONE_TEST_LOG_INTERVAL_MS * 1000LL;

    const int64_t now_us = esp_timer_get_time();

    if (!bsp_microphone_is_ready()) {
        return;
    }

#if CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
    app_mic_stream_init(bsp_microphone_get_sample_rate_hz(), bsp_microphone_get_channels());
    app_mic_stream_log_status_if_due();
#endif

    if (!s_mic_test.started) {
        s_mic_test.channels = bsp_microphone_get_channels();
        s_mic_test.frame_bytes = (uint32_t)bsp_microphone_frame_bytes();
        s_mic_test.chunk_bytes = (uint32_t)app_mic_test_chunk_bytes();
        s_mic_test.chunk_frames = s_mic_test.frame_bytes == 0U ? 0U :
                                  s_mic_test.chunk_bytes / s_mic_test.frame_bytes;
        s_mic_test.next_read_us = now_us + chunk_interval_us;
        s_mic_test.stats_window_start_us = now_us;

        if (s_mic_test.chunk_bytes == 0U || s_mic_test.chunk_frames == 0U) {
            ESP_LOGE(TAG, "Mic test config invalid: chunk_bytes=%lu frame_bytes=%lu",
                     (unsigned long)s_mic_test.chunk_bytes,
                     (unsigned long)s_mic_test.frame_bytes);
            return;
        }

        s_mic_test.started = true;
        ESP_LOGI(TAG,
                 "Mic test started: chunk=%luB frames=%lu frame_bytes=%lu rate=%lu channels=%u",
                 (unsigned long)s_mic_test.chunk_bytes,
                 (unsigned long)s_mic_test.chunk_frames,
                 (unsigned long)s_mic_test.frame_bytes,
                 (unsigned long)bsp_microphone_get_sample_rate_hz(),
                 (unsigned int)s_mic_test.channels);
    }

    if (now_us < s_mic_test.next_read_us) {
        return;
    }

    int64_t read_deadline_us = now_us;
    while (read_deadline_us >= s_mic_test.next_read_us) {
        size_t bytes_read = 0U;
        esp_err_t ret = bsp_microphone_read(
            pcm_buffer, s_mic_test.chunk_bytes, &bytes_read, 0U);
        s_mic_test.last_bytes_read = (uint32_t)bytes_read;
        if (ret == ESP_OK && bytes_read >= s_mic_test.frame_bytes) {
            if (bytes_read < s_mic_test.chunk_bytes) {
                s_mic_test.partial_read_count++;
            }
            if ((bytes_read % s_mic_test.frame_bytes) != 0U) {
                s_mic_test.misaligned_read_count++;
            }
            const size_t frame_count = bytes_read / s_mic_test.frame_bytes;
            app_mic_test_accumulate((const int16_t *)pcm_buffer, frame_count, s_mic_test.channels);
#if CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
            app_mic_stream_send(pcm_buffer, bytes_read);
#endif
            s_mic_test.chunk_count++;
        } else {
            if (ret != ESP_ERR_TIMEOUT) {
                s_mic_test.read_fail_count++;
            }
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Mic test read failed: %s", esp_err_to_name(ret));
            }
            break;
        }

        s_mic_test.next_read_us += chunk_interval_us;
        read_deadline_us = esp_timer_get_time();
    }

    const int64_t stats_now_us = esp_timer_get_time();
    if ((stats_now_us - s_mic_test.stats_window_start_us) >= stats_interval_us) {
        app_mic_test_log_stats(stats_now_us);
    }
}
#endif

#if CONFIG_COLLAR_SPEAKER_TEST_ENABLE
typedef struct {
    bool started;
    bool completion_logged;
    uint32_t chunk_bytes;
    uint32_t frames_per_chunk;
    uint32_t tone_frames;
    uint32_t gap_frames;
    uint32_t cycle_frames;
    uint32_t completed_loops;
    uint64_t frame_cursor;
    uint32_t phase_acc;
    int64_t next_write_us;
} app_speaker_test_state_t;

static app_speaker_test_state_t s_speaker_test;
static const uint16_t s_speaker_test_notes_hz[] = {440U, 554U, 659U, 880U};

static size_t app_speaker_test_chunk_bytes(void)
{
    const uint32_t sample_rate = bsp_speaker_get_sample_rate_hz();
    const uint32_t bytes_per_frame = 4U;
    uint32_t chunk_bytes =
        (sample_rate * bytes_per_frame * CONFIG_COLLAR_SPEAKER_TEST_CHUNK_MS) / 1000U;

    chunk_bytes = (chunk_bytes / bytes_per_frame) * bytes_per_frame;
    if (chunk_bytes == 0U) {
        chunk_bytes = bytes_per_frame;
    }

    if (chunk_bytes > APP_SPEAKER_TEST_MAX_CHUNK_BYTES) {
        return 0U;
    }

    return (size_t)chunk_bytes;
}

static int16_t app_speaker_test_triangle_sample(app_speaker_test_state_t *state, uint16_t freq_hz)
{
    const uint32_t sample_rate = bsp_speaker_get_sample_rate_hz();
    const uint32_t volume = (uint32_t)CONFIG_COLLAR_SPEAKER_TEST_VOLUME_PERCENT;
    const uint32_t amplitude = (28000U * volume) / 100U;
    const uint32_t phase_step = (uint32_t)(((uint64_t)freq_hz << 32) / sample_rate);
    const uint32_t phase = state->phase_acc;
    const uint32_t ramp = phase >> 15;
    const int32_t triangle = (ramp < 65536U) ? (int32_t)ramp : (int32_t)(131071U - ramp);
    const int32_t centered = triangle - 32768;
    const int32_t scaled = (centered * (int32_t)amplitude) / 32768;

    state->phase_acc += phase_step;
    return (int16_t)scaled;
}

static void app_speaker_test_fill_pcm(app_speaker_test_state_t *state, uint8_t *buffer, size_t len)
{
    const uint32_t bytes_per_frame = 4U;
    const size_t frames = len / bytes_per_frame;
    const uint32_t segment_frames = state->tone_frames + state->gap_frames;
    const uint32_t note_count = sizeof(s_speaker_test_notes_hz) / sizeof(s_speaker_test_notes_hz[0]);
    const uint32_t cycle_frames = segment_frames * note_count;

    memset(buffer, 0, len);
    if (segment_frames == 0U || cycle_frames == 0U) {
        return;
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        const uint32_t cycle_pos = (uint32_t)(state->frame_cursor % cycle_frames);
        const uint32_t note_index = cycle_pos / segment_frames;
        const uint32_t segment_pos = cycle_pos % segment_frames;
        int16_t sample = 0;

        if (segment_pos < state->tone_frames) {
            if (segment_pos == 0U) {
                state->phase_acc = 0U;
            }
            sample = app_speaker_test_triangle_sample(state, s_speaker_test_notes_hz[note_index]);
        } else {
            state->phase_acc = 0U;
        }

        const size_t offset = frame * bytes_per_frame;
        buffer[offset] = (uint8_t)(sample & 0xff);
        buffer[offset + 1U] = (uint8_t)((sample >> 8) & 0xff);
        buffer[offset + 2U] = (uint8_t)(sample & 0xff);
        buffer[offset + 3U] = (uint8_t)((sample >> 8) & 0xff);
        state->frame_cursor++;
    }
}

static void app_speaker_test_tick(void)
{
    static uint8_t pcm_buffer[APP_SPEAKER_TEST_MAX_CHUNK_BYTES];

    const int64_t now_us = esp_timer_get_time();

    if (!bsp_speaker_is_ready()) {
        return;
    }

    if (!s_speaker_test.started) {
        s_speaker_test.chunk_bytes = (uint32_t)app_speaker_test_chunk_bytes();
        s_speaker_test.frames_per_chunk = s_speaker_test.chunk_bytes / 4U;
        s_speaker_test.tone_frames =
            (bsp_speaker_get_sample_rate_hz() * CONFIG_COLLAR_SPEAKER_TEST_TONE_MS) / 1000U;
        s_speaker_test.gap_frames =
            (bsp_speaker_get_sample_rate_hz() * CONFIG_COLLAR_SPEAKER_TEST_GAP_MS) / 1000U;
        s_speaker_test.cycle_frames =
            (s_speaker_test.tone_frames + s_speaker_test.gap_frames) *
            (sizeof(s_speaker_test_notes_hz) / sizeof(s_speaker_test_notes_hz[0]));
        s_speaker_test.next_write_us = now_us;

        if (s_speaker_test.chunk_bytes == 0U ||
            s_speaker_test.frames_per_chunk == 0U ||
            s_speaker_test.tone_frames == 0U ||
            s_speaker_test.cycle_frames == 0U) {
            ESP_LOGE(TAG,
                     "Speaker self-test config invalid: chunk=%lu tone_frames=%lu gap_frames=%lu cycle_frames=%lu",
                     (unsigned long)s_speaker_test.chunk_bytes,
                     (unsigned long)s_speaker_test.tone_frames,
                     (unsigned long)s_speaker_test.gap_frames,
                     (unsigned long)s_speaker_test.cycle_frames);
            return;
        }

        s_speaker_test.started = true;
        ESP_LOGI(TAG,
                 "Speaker self-test started: chunk=%luB sample_rate=%lu notes=440,554,659,880Hz loops=%u",
                 (unsigned long)s_speaker_test.chunk_bytes,
                 (unsigned long)bsp_speaker_get_sample_rate_hz(),
                 (unsigned int)APP_SPEAKER_TEST_MAX_LOOPS);
    }

    if (s_speaker_test.completed_loops >= APP_SPEAKER_TEST_MAX_LOOPS) {
        if (!s_speaker_test.completion_logged) {
            s_speaker_test.completion_logged = true;
            ESP_LOGI(TAG, "Speaker self-test complete: loops=%u",
                     (unsigned int)s_speaker_test.completed_loops);
        }
        return;
    }

    if (now_us < s_speaker_test.next_write_us) {
        return;
    }

    if ((s_speaker_test.frame_cursor + s_speaker_test.frames_per_chunk) > s_speaker_test.cycle_frames) {
        s_speaker_test.frame_cursor = 0U;
        s_speaker_test.phase_acc = 0U;
        s_speaker_test.completed_loops++;

        if (s_speaker_test.completed_loops >= APP_SPEAKER_TEST_MAX_LOOPS) {
            if (!s_speaker_test.completion_logged) {
                s_speaker_test.completion_logged = true;
                ESP_LOGI(TAG, "Speaker self-test complete: loops=%u",
                         (unsigned int)s_speaker_test.completed_loops);
            }
            return;
        }
    }

    app_speaker_test_fill_pcm(&s_speaker_test, pcm_buffer, s_speaker_test.chunk_bytes);

    size_t bytes_written = 0U;
    esp_err_t ret = bsp_speaker_write(
        pcm_buffer,
        s_speaker_test.chunk_bytes,
        &bytes_written,
        CONFIG_COLLAR_SPEAKER_TEST_CHUNK_MS + 20);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Speaker self-test write failed: %s", esp_err_to_name(ret));
        s_speaker_test.next_write_us = now_us + 200000LL;
        return;
    }

    if (bytes_written != s_speaker_test.chunk_bytes) {
        ESP_LOGW(TAG, "Speaker self-test short write: %u/%lu",
                 (unsigned int)bytes_written,
                 (unsigned long)s_speaker_test.chunk_bytes);
    }

    s_speaker_test.next_write_us =
        now_us + ((int64_t)CONFIG_COLLAR_SPEAKER_TEST_CHUNK_MS * 1000LL);
}
#endif

static void collar_app_task(void *arg)
{
    (void)arg;

    uint32_t heartbeat_seq = 0;
    int64_t next_heartbeat_us = esp_timer_get_time();

    for (;;) {
        const int64_t now_us = esp_timer_get_time();

#if CONFIG_COLLAR_CONVERSATION_SELF_TEST_ENABLE
        app_self_test_tick();
#endif
#if CONFIG_COLLAR_SPEAKER_TEST_ENABLE
        app_speaker_test_tick();
#endif
#if CONFIG_COLLAR_MICROPHONE_TEST_ENABLE
        app_mic_test_tick();
#endif

        if (now_us >= next_heartbeat_us) {
            heartbeat_seq++;

            const kernel_msg_t msg = {
                .topic = KERNEL_TOPIC_APP_HEARTBEAT,
                .source = KERNEL_SOURCE_APP,
                .value = heartbeat_seq,
                .timestamp_us = now_us,
            };
            (void)kernel_msgbus_publish(&msg, pdMS_TO_TICKS(10));

            if ((heartbeat_seq % 10U) == 0U) {
                kernel_trace_counter("app_heartbeat", heartbeat_seq);
            }

            ESP_LOGD(TAG, "Collar app alive seq=%lu", (unsigned long)heartbeat_seq);
            next_heartbeat_us = now_us + ((int64_t)APP_HEARTBEAT_PERIOD_MS * 1000LL);
        }

        vTaskDelay(pdMS_TO_TICKS(APP_LOOP_PERIOD_MS));
    }
}

esp_err_t app_manager_start(void)
{
    if (s_app_started) {
        return ESP_OK;
    }

    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        collar_app_task,
        "collar_app",
        APP_MANAGER_STACK_WORDS,
        NULL,
        APP_MANAGER_PRIORITY,
        s_app_stack,
        &s_app_tcb,
        APP_MANAGER_CORE
    );

    if (task_handle == NULL) {
        return ESP_FAIL;
    }

    s_app_started = true;
    kernel_trace_boot("app manager started");
    return ESP_OK;
}
