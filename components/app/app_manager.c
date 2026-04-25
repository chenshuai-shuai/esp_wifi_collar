#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "app/app_manager.h"
#include "app/dialog_orchestrator.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"


#include "audio_dsp/audio_denoise.h"
#include "bsp/microphone_input.h"
#include "bsp/speaker_output.h"
#include "conversation/conversation_client.h"
#include "kernel/kernel_msgbus.h"
#include "kernel/kernel_trace.h"
#include "services/wifi_service.h"

#define APP_MANAGER_STACK_WORDS           4096
#define APP_MANAGER_PRIORITY              4
#define APP_MANAGER_CORE                  tskNO_AFFINITY
#define APP_CONSOLE_STACK_WORDS           4096
#define APP_MIC_UPLINK_STACK_WORDS        4096
#define APP_HEARTBEAT_PERIOD_MS           1000
#define APP_LOOP_PERIOD_MS                10
#define APP_SELF_TEST_TAIL_SILENCE_CHUNKS 5U
#define APP_SELF_TEST_MAX_CHUNK_BYTES     2048
#define APP_MIC_TEST_MAX_CHUNK_BYTES      4096
#define APP_SPEAKER_TEST_MAX_CHUNK_BYTES  2048
#define APP_SPEAKER_TEST_MAX_LOOPS        1U
#define APP_SPEAKER_TEST_MAX_CATCHUP_WRITES 2U
#define APP_MIC_STREAM_STATUS_LOG_US      5000000LL
#define APP_AUDIO_STATUS_LOG_US           3000000LL
#define APP_SPEAKER_TEST_ATTACK_MS        10U
#define APP_SPEAKER_TEST_RELEASE_MS       24U
#define APP_SPEAKER_TEST_HARMONIC_PCT     14U
#define APP_UPLINK_BACKPRESSURE_RETRY_MS  5U
#define APP_UPLINK_BACKPRESSURE_MAX_RETRY 1U
#define APP_CONV_UPLINK_CHUNK_MS          40U

#ifdef CONFIG_COLLAR_MICROPHONE_DENOISE_VAD_ENABLE
#define APP_MIC_DENOISE_VAD_ENABLED true
#else
#define APP_MIC_DENOISE_VAD_ENABLED false
#endif

static const char *TAG = "app_mgr";

static StaticTask_t s_app_tcb;
static StackType_t s_app_stack[APP_MANAGER_STACK_WORDS];
static StaticTask_t s_console_tcb;
static StackType_t s_console_stack[APP_CONSOLE_STACK_WORDS];
static bool s_app_started;

typedef enum {

    APP_AUDIO_MODE_BOOT_CHIME = 0,
    APP_AUDIO_MODE_MIC = 1,
} app_audio_mode_t;

typedef struct {
    bool initialized;
    bool boot_chime_done;
    app_audio_mode_t mode;
    int64_t mode_started_us;
} app_audio_runtime_state_t;

static app_audio_runtime_state_t s_audio_runtime;


#if CONFIG_COLLAR_MICROPHONE_TEST_ENABLE
typedef struct {
    bool started;
    bool denoise_init_failed;
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
    uint32_t denoise_fail_count;
    uint32_t last_bytes_read;
    uint32_t left_peak;
    uint32_t right_peak;
    uint64_t left_abs_sum;
    uint64_t right_abs_sum;
    uint64_t sample_count;
    uint64_t vad_block_count;
    uint64_t total_chunk_count;
    uint64_t total_read_fail_count;
    uint64_t total_partial_read_count;
    uint64_t total_misaligned_read_count;
    uint64_t total_denoise_fail_count;
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

static void app_mic_stream_deinit(void)
{
    if (s_mic_stream.sock >= 0) {
        close(s_mic_stream.sock);
    }
    memset(&s_mic_stream, 0, sizeof(s_mic_stream));
    s_mic_stream.sock = -1;
}

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
#if CONFIG_COLLAR_MICROPHONE_DENOISE_ENABLE
        ESP_LOGD(TAG,
                 "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu dn_fail=%lu last_bytes=%lu udp=%lu/%lu fail=%lu vad_blocks=%llu no samples",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.denoise_fail_count,
                 (unsigned long)s_mic_test.last_bytes_read,
                 (unsigned long)s_mic_stream.tx_chunks,
                 (unsigned long)s_mic_stream.tx_bytes,
                 (unsigned long)s_mic_stream.tx_failures,
                 (unsigned long long)s_mic_test.vad_block_count);
#else
        ESP_LOGD(TAG,
                 "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu last_bytes=%lu udp=%lu/%lu fail=%lu no samples",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.last_bytes_read,
                 (unsigned long)s_mic_stream.tx_chunks,
                 (unsigned long)s_mic_stream.tx_bytes,
                 (unsigned long)s_mic_stream.tx_failures);
#endif
#else
#if CONFIG_COLLAR_MICROPHONE_DENOISE_ENABLE
        ESP_LOGD(TAG, "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu dn_fail=%lu last_bytes=%lu vad_blocks=%llu no samples",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.denoise_fail_count,
                 (unsigned long)s_mic_test.last_bytes_read,
                 (unsigned long long)s_mic_test.vad_block_count);
#else
        ESP_LOGD(TAG, "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu last_bytes=%lu no samples",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.last_bytes_read);
#endif
#endif
    } else {
        const uint32_t left_avg_abs =
            (uint32_t)(s_mic_test.left_abs_sum / s_mic_test.sample_count);
        const uint32_t right_avg_abs =
            (uint32_t)(s_mic_test.right_abs_sum / s_mic_test.sample_count);
#if CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
#if CONFIG_COLLAR_MICROPHONE_DENOISE_ENABLE
        ESP_LOGD(TAG,
                 "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu dn_fail=%lu last_bytes=%lu udp=%lu/%lu fail=%lu vad_blocks=%llu frames=%llu L(avg_abs=%lu peak=%lu) R(avg_abs=%lu peak=%lu)",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.denoise_fail_count,
                 (unsigned long)s_mic_test.last_bytes_read,
                 (unsigned long)s_mic_stream.tx_chunks,
                 (unsigned long)s_mic_stream.tx_bytes,
                 (unsigned long)s_mic_stream.tx_failures,
                 (unsigned long long)s_mic_test.vad_block_count,
                 (unsigned long long)s_mic_test.sample_count,
                 (unsigned long)left_avg_abs,
                 (unsigned long)s_mic_test.left_peak,
                 (unsigned long)right_avg_abs,
                 (unsigned long)s_mic_test.right_peak);
#else
        ESP_LOGD(TAG,
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
#endif
#else
#if CONFIG_COLLAR_MICROPHONE_DENOISE_ENABLE
        ESP_LOGD(TAG,
                 "Mic test: chunks=%lu fail=%lu partial=%lu misaligned=%lu dn_fail=%lu last_bytes=%lu vad_blocks=%llu frames=%llu L(avg_abs=%lu peak=%lu) R(avg_abs=%lu peak=%lu)",
                 (unsigned long)s_mic_test.chunk_count,
                 (unsigned long)s_mic_test.read_fail_count,
                 (unsigned long)s_mic_test.partial_read_count,
                 (unsigned long)s_mic_test.misaligned_read_count,
                 (unsigned long)s_mic_test.denoise_fail_count,
                 (unsigned long)s_mic_test.last_bytes_read,
                 (unsigned long long)s_mic_test.vad_block_count,
                 (unsigned long long)s_mic_test.sample_count,
                 (unsigned long)left_avg_abs,
                 (unsigned long)s_mic_test.left_peak,
                 (unsigned long)right_avg_abs,
                 (unsigned long)s_mic_test.right_peak);
#else
        ESP_LOGD(TAG,
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
#endif
    }

    s_mic_test.chunk_count = 0U;
    s_mic_test.read_fail_count = 0U;
    s_mic_test.partial_read_count = 0U;
    s_mic_test.misaligned_read_count = 0U;
    s_mic_test.denoise_fail_count = 0U;
    s_mic_test.last_bytes_read = 0U;
    s_mic_test.left_peak = 0U;
    s_mic_test.right_peak = 0U;
    s_mic_test.left_abs_sum = 0U;
    s_mic_test.right_abs_sum = 0U;
    s_mic_test.sample_count = 0U;
    s_mic_test.vad_block_count = 0U;
    s_mic_test.stats_window_start_us = now_us;
}

static void app_mic_test_reset(void)
{
    memset(&s_mic_test, 0, sizeof(s_mic_test));
}

static void app_mic_test_tick(void)
{
    static uint8_t pcm_buffer[APP_MIC_TEST_MAX_CHUNK_BYTES];
    static bool denoise_suppressed_logged = false;
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
#endif
#if CONFIG_COLLAR_MICROPHONE_DENOISE_ENABLE
    const bool suppress_denoise_for_conv = conversation_client_is_configured();
    if (suppress_denoise_for_conv) {
        if (audio_denoise_is_ready()) {
            audio_denoise_deinit();
        }
        if (!denoise_suppressed_logged) {
            denoise_suppressed_logged = true;
            ESP_LOGI(TAG, "Mic denoise suppressed while conversation client is configured "
                          "(reserve RAM for gRPC/manual-h2c)");
        }
        s_mic_test.denoise_init_failed = false;
    } else {
        denoise_suppressed_logged = false;
        if (!audio_denoise_is_ready()) {
            const audio_denoise_config_t denoise_config = {
                .sample_rate_hz = bsp_microphone_get_sample_rate_hz(),
                .channels = bsp_microphone_get_channels(),
                .frame_ms = CONFIG_COLLAR_MICROPHONE_DENOISE_FRAME_MS,
                .noise_suppress_db = CONFIG_COLLAR_MICROPHONE_DENOISE_NOISE_SUPPRESS_DB,
                .vad_enabled = APP_MIC_DENOISE_VAD_ENABLED,
            };
            esp_err_t denoise_ret = audio_denoise_init(&denoise_config);
            if (denoise_ret != ESP_OK && !s_mic_test.denoise_init_failed) {
                s_mic_test.denoise_init_failed = true;
                ESP_LOGE(TAG, "Mic denoise init failed: %s", esp_err_to_name(denoise_ret));
            }
        } else {
            s_mic_test.denoise_init_failed = false;
        }
    }
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
                 "Mic test started: chunk=%luB frames=%lu frame_bytes=%lu rate=%lu channels=%u denoise=%s",
                 (unsigned long)s_mic_test.chunk_bytes,
                 (unsigned long)s_mic_test.chunk_frames,
                 (unsigned long)s_mic_test.frame_bytes,
                 (unsigned long)bsp_microphone_get_sample_rate_hz(),
                 (unsigned int)s_mic_test.channels,
#if CONFIG_COLLAR_MICROPHONE_DENOISE_ENABLE
                 "speexdsp"
#else
                 "off"
#endif
        );
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
                s_mic_test.total_partial_read_count++;
            }
            if ((bytes_read % s_mic_test.frame_bytes) != 0U) {
                s_mic_test.misaligned_read_count++;
                s_mic_test.total_misaligned_read_count++;
            }
            const size_t frame_count = bytes_read / s_mic_test.frame_bytes;
#if CONFIG_COLLAR_MICROPHONE_DENOISE_ENABLE
            if (audio_denoise_is_ready()) {
                size_t speech_blocks = 0U;
                esp_err_t denoise_ret = audio_denoise_process_interleaved(
                    (int16_t *)pcm_buffer, frame_count, &speech_blocks);
                if (denoise_ret == ESP_OK) {
                    s_mic_test.vad_block_count += speech_blocks;
                } else {
                    s_mic_test.denoise_fail_count++;
                    s_mic_test.total_denoise_fail_count++;
                    if (s_mic_test.denoise_fail_count <= 3U ||
                        (s_mic_test.denoise_fail_count % 50U) == 0U) {
                        ESP_LOGW(TAG, "Mic denoise failed: %s", esp_err_to_name(denoise_ret));
                    }
                }
            }
#endif
            app_mic_test_accumulate((const int16_t *)pcm_buffer, frame_count, s_mic_test.channels);
#if CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
            app_mic_stream_send(pcm_buffer, bytes_read);
#endif
            s_mic_test.chunk_count++;
            s_mic_test.total_chunk_count++;
        } else {
            if (ret != ESP_ERR_TIMEOUT) {
                s_mic_test.read_fail_count++;
                s_mic_test.total_read_fail_count++;
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
    uint32_t write_count;
    uint32_t short_write_count;
    uint32_t write_fail_count;
    uint32_t last_bytes_written;
    uint32_t total_bytes_written;
    uint64_t frame_cursor;
    uint32_t phase_acc;
    int64_t next_write_us;
} app_speaker_test_state_t;

static app_speaker_test_state_t s_speaker_test;
static const uint16_t s_speaker_test_notes_hz[] = {440U, 554U, 659U, 880U};
static const int16_t s_speaker_test_sine_quarter_q15[] = {
    0, 1608, 3212, 4808, 6393, 7962, 9512, 11039, 12539, 14010, 15446,
    16846, 18204, 19519, 20787, 22005, 23170, 24279, 25329, 26319, 27245,
    28105, 28898, 29621, 30273, 30852, 31356, 31785, 32137, 32412, 32609,
    32728, 32767
};

static bool app_speaker_test_continuous(void)
{
    return APP_SPEAKER_TEST_MAX_LOOPS == 0U;
}

static void app_speaker_test_reset(void)
{
    memset(&s_speaker_test, 0, sizeof(s_speaker_test));
}

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

static int16_t app_speaker_test_sine_q15(uint32_t phase_acc)
{
    const uint32_t quadrant = phase_acc >> 30;
    const uint32_t quadrant_phase = (phase_acc >> 25) & 0x1FU;
    const uint32_t mirror_index = 32U - quadrant_phase;
    const int16_t *table = s_speaker_test_sine_quarter_q15;

    switch (quadrant) {
    case 0U:
        return table[quadrant_phase];
    case 1U:
        return table[mirror_index];
    case 2U:
        return (int16_t)(-table[quadrant_phase]);
    default:
        return (int16_t)(-table[mirror_index]);
    }
}

static int16_t app_speaker_test_tone_sample(app_speaker_test_state_t *state, uint16_t freq_hz)
{
    const uint32_t sample_rate = bsp_speaker_get_sample_rate_hz();
    const uint32_t phase_step = (uint32_t)(((uint64_t)freq_hz << 32) / sample_rate);
    const uint32_t harmonic_step = phase_step << 1;
    const int32_t amplitude = (22000 * CONFIG_COLLAR_SPEAKER_TEST_VOLUME_PERCENT) / 100;
    const int32_t fundamental = app_speaker_test_sine_q15(state->phase_acc);
    const int32_t harmonic = app_speaker_test_sine_q15(state->phase_acc << 1);
    int32_t mixed_q15 =
        fundamental + ((harmonic * (int32_t)APP_SPEAKER_TEST_HARMONIC_PCT) / 100);
    int32_t sample = (mixed_q15 * amplitude) / 32767;

    state->phase_acc += phase_step;
    (void)harmonic_step;
    return sample > INT16_MAX ? INT16_MAX :
           sample < INT16_MIN ? INT16_MIN :
           (int16_t)sample;
}

static void app_speaker_test_fill_pcm(app_speaker_test_state_t *state, uint8_t *buffer, size_t len)
{
    const uint32_t bytes_per_frame = 4U;
    const size_t frames = len / bytes_per_frame;
    const uint32_t segment_frames = state->tone_frames + state->gap_frames;
    const uint32_t note_count = sizeof(s_speaker_test_notes_hz) / sizeof(s_speaker_test_notes_hz[0]);
    const uint32_t cycle_frames = segment_frames * note_count;
    const uint32_t sample_rate = bsp_speaker_get_sample_rate_hz();
    const uint32_t attack_frames = (sample_rate * APP_SPEAKER_TEST_ATTACK_MS) / 1000U;
    const uint32_t release_frames = (sample_rate * APP_SPEAKER_TEST_RELEASE_MS) / 1000U;

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
            sample = app_speaker_test_tone_sample(state, s_speaker_test_notes_hz[note_index]);

            uint32_t envelope_q15 = 32767U;
            if (attack_frames > 0U && segment_pos < attack_frames) {
                envelope_q15 = (uint32_t)(((uint64_t)segment_pos * 32767U) / attack_frames);
            }

            const uint32_t release_start =
                (release_frames < state->tone_frames) ? (state->tone_frames - release_frames) : 0U;
            if (release_frames > 0U && segment_pos >= release_start) {
                const uint32_t frames_left = state->tone_frames - segment_pos;
                const uint32_t release_q15 =
                    (uint32_t)(((uint64_t)frames_left * 32767U) / release_frames);
                if (release_q15 < envelope_q15) {
                    envelope_q15 = release_q15;
                }
            }

            sample = (int16_t)(((int32_t)sample * (int32_t)envelope_q15) / 32767);
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
    const int64_t chunk_interval_us =
        (int64_t)CONFIG_COLLAR_SPEAKER_TEST_CHUNK_MS * 1000LL;

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
                 "Speaker self-test started: chunk=%luB sample_rate=%lu notes=440,554,659,880Hz loops=%s tone=sine+harmonic env=%ums/%ums volume=%u%%",
                 (unsigned long)s_speaker_test.chunk_bytes,
                 (unsigned long)bsp_speaker_get_sample_rate_hz(),
                 app_speaker_test_continuous() ? "continuous" : "finite",
                 (unsigned int)APP_SPEAKER_TEST_ATTACK_MS,
                 (unsigned int)APP_SPEAKER_TEST_RELEASE_MS,
                 (unsigned int)CONFIG_COLLAR_SPEAKER_TEST_VOLUME_PERCENT);
    }

    if (!app_speaker_test_continuous()) {
#if APP_SPEAKER_TEST_MAX_LOOPS > 0U
        if (s_speaker_test.completed_loops >= APP_SPEAKER_TEST_MAX_LOOPS) {
            if (!s_speaker_test.completion_logged) {
                s_speaker_test.completion_logged = true;
                ESP_LOGI(TAG, "Speaker self-test complete: loops=%u",
                         (unsigned int)s_speaker_test.completed_loops);
            }
            return;
        }
#endif
    }

    if (now_us < s_speaker_test.next_write_us) {
        return;
    }

    int64_t write_deadline_us = now_us;
    uint32_t catchup_writes = 0U;
    while (write_deadline_us >= s_speaker_test.next_write_us) {
        if ((s_speaker_test.frame_cursor + s_speaker_test.frames_per_chunk) > s_speaker_test.cycle_frames) {
            s_speaker_test.frame_cursor = 0U;
            s_speaker_test.phase_acc = 0U;
            s_speaker_test.completed_loops++;

            if (!app_speaker_test_continuous()) {
#if APP_SPEAKER_TEST_MAX_LOOPS > 0U
                if (s_speaker_test.completed_loops >= APP_SPEAKER_TEST_MAX_LOOPS) {
                    if (!s_speaker_test.completion_logged) {
                        s_speaker_test.completion_logged = true;
                        ESP_LOGI(TAG, "Speaker self-test complete: loops=%u",
                                 (unsigned int)s_speaker_test.completed_loops);
                    }
                    return;
                }
#endif
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
            s_speaker_test.write_fail_count++;
            ESP_LOGW(TAG, "Speaker self-test write failed: %s", esp_err_to_name(ret));
            s_speaker_test.next_write_us = now_us + 200000LL;
            return;
        }

        s_speaker_test.write_count++;
        s_speaker_test.last_bytes_written = (uint32_t)bytes_written;
        s_speaker_test.total_bytes_written += (uint32_t)bytes_written;
        if (bytes_written != s_speaker_test.chunk_bytes) {
            s_speaker_test.short_write_count++;
            ESP_LOGW(TAG, "Speaker self-test short write: %u/%lu",
                     (unsigned int)bytes_written,
                     (unsigned long)s_speaker_test.chunk_bytes);
        }

        s_speaker_test.next_write_us += chunk_interval_us;
        catchup_writes++;
        if (catchup_writes >= APP_SPEAKER_TEST_MAX_CATCHUP_WRITES) {
            s_speaker_test.next_write_us = esp_timer_get_time() + chunk_interval_us;
            break;
        }
        write_deadline_us = esp_timer_get_time();
    }
}
#endif

static void app_audio_log_status_if_due(int64_t now_us)
{
    static int64_t s_last_audio_status_log_us;

    if ((now_us - s_last_audio_status_log_us) < APP_AUDIO_STATUS_LOG_US) {
        return;
    }
    s_last_audio_status_log_us = now_us;

    const UBaseType_t app_stack_hwm = uxTaskGetStackHighWaterMark(NULL);
    const char *mode_state =
        s_audio_runtime.mode == APP_AUDIO_MODE_BOOT_CHIME ? "boot-chime" : "mic";
    const char *mic_state = bsp_microphone_is_ready() ? "ready" : "off";
    const char *speaker_state = bsp_speaker_is_ready() ? "ready" : "off";

#if CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
    const char *udp_state = !wifi_service_sta_ready() ? "waiting" :
        ((s_mic_stream.ready && s_mic_stream.sock >= 0) ? "ready" : "not-open");
#else
    const char *udp_state = "off";
#endif

#if CONFIG_COLLAR_SPEAKER_TEST_ENABLE
    ESP_LOGD(TAG,
             "Audio mode=%s: mic=%s spk=%s udp=%s app_stack=%u mic(chunks=%llu fail=%llu partial=%llu dn=%llu) spk(writes=%u short=%u fail=%u loops=%u/%s bytes=%u)",
             mode_state,
             mic_state,
             speaker_state,
             udp_state,
             (unsigned int)app_stack_hwm,
             (unsigned long long)s_mic_test.total_chunk_count,
             (unsigned long long)s_mic_test.total_read_fail_count,
             (unsigned long long)s_mic_test.total_partial_read_count,
             (unsigned long long)s_mic_test.total_denoise_fail_count,
             (unsigned int)s_speaker_test.write_count,
             (unsigned int)s_speaker_test.short_write_count,
             (unsigned int)s_speaker_test.write_fail_count,
             (unsigned int)s_speaker_test.completed_loops,
             app_speaker_test_continuous() ? "inf" : "finite",
             (unsigned int)s_speaker_test.total_bytes_written);
#else
    ESP_LOGD(TAG,
             "Audio mode=%s: mic=%s spk=%s udp=%s app_stack=%u mic(chunks=%llu fail=%llu partial=%llu dn=%llu) spk(disabled)",
             mode_state,
             mic_state,
             speaker_state,
             udp_state,
             (unsigned int)app_stack_hwm,
             (unsigned long long)s_mic_test.total_chunk_count,
             (unsigned long long)s_mic_test.total_read_fail_count,
             (unsigned long long)s_mic_test.total_partial_read_count,
             (unsigned long long)s_mic_test.total_denoise_fail_count);
#endif
}

static void app_audio_switch_to_mic(void)
{
    /* Keep the speaker powered up during mic mode so that server
     * downlink audio can be rendered alongside uplink capture
     * (full-duplex conversation driven via UART console). */
    if (!bsp_speaker_is_ready()) {
        esp_err_t spk_ret = bsp_speaker_init();
        if (spk_ret != ESP_OK) {
            ESP_LOGW(TAG, "Speaker init failed during mic-mode switch: %s",
                     esp_err_to_name(spk_ret));
        }
    }

#if CONFIG_COLLAR_SPEAKER_TEST_ENABLE
    app_speaker_test_reset();
#endif

#if CONFIG_COLLAR_MICROPHONE_TEST_ENABLE && CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
    app_mic_stream_deinit();
#endif
#if CONFIG_COLLAR_MICROPHONE_DENOISE_ENABLE
    audio_denoise_deinit();
#endif
#if CONFIG_COLLAR_MICROPHONE_TEST_ENABLE
    app_mic_test_reset();
#endif

    esp_err_t ret = bsp_microphone_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio mode switch failed to init microphone: %s", esp_err_to_name(ret));
        return;
    }

    s_audio_runtime.mode = APP_AUDIO_MODE_MIC;
    s_audio_runtime.boot_chime_done = true;
    s_audio_runtime.mode_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Audio mode -> mic: continuous_capture=on boot_chime=done resources=reclaimed");
    vTaskDelay(pdMS_TO_TICKS(20));
}

#if CONFIG_COLLAR_SPEAKER_TEST_ENABLE
static void app_audio_switch_to_boot_chime(void)
{
#if CONFIG_COLLAR_MICROPHONE_STREAM_ENABLE
    app_mic_stream_deinit();
#endif
#if CONFIG_COLLAR_MICROPHONE_DENOISE_ENABLE
    audio_denoise_deinit();
#endif
    app_mic_test_reset();
    bsp_microphone_deinit();
    app_speaker_test_reset();

    esp_err_t ret = bsp_speaker_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Boot chime init failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Boot chime skipped; falling back to continuous microphone mode");
        app_audio_switch_to_mic();
        return;
    }

    s_audio_runtime.mode = APP_AUDIO_MODE_BOOT_CHIME;
    s_audio_runtime.mode_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Boot chime started: speaker=on microphone=off startup_once=yes");
    vTaskDelay(pdMS_TO_TICKS(20));
}
#endif /* CONFIG_COLLAR_SPEAKER_TEST_ENABLE */

static void app_audio_mode_tick(int64_t now_us)
{
    (void)now_us;

    if (!s_audio_runtime.initialized) {
        s_audio_runtime.initialized = true;
#if CONFIG_COLLAR_SPEAKER_TEST_ENABLE
        app_audio_switch_to_boot_chime();
#else
        app_audio_switch_to_mic();
#endif
        return;
    }

    if (s_audio_runtime.mode == APP_AUDIO_MODE_MIC) {
        return;
    }

#if CONFIG_COLLAR_SPEAKER_TEST_ENABLE
    if (s_speaker_test.started &&
        s_speaker_test.completed_loops >= APP_SPEAKER_TEST_MAX_LOOPS) {
        ESP_LOGI(TAG, "Boot chime complete: loops=%u speaker=off microphone=on",
                 (unsigned int)s_speaker_test.completed_loops);
        app_audio_switch_to_mic();
    }
#endif
}

/* ------------------------------------------------------------------
 * Console command parser: reads line-by-line from UART0 (the same UART
 * idf_monitor is hooked up to) and recognises two commands:
 *
 *   ESP:CONV_START
 *       Open a new streaming conversation: register speaker listener
 *       if not yet registered, call conversation_client_start_conversation,
 *       kick off TALKING, and continuously push mic PCM until
 *       ESP:CONV_STOP:<sid> arrives.
 *
 *   ESP:CONV_STOP:<session_id>
 *       Fire a one-shot unary EndConversation RPC with the given
 *       session_id (as reported by the server backend), then tear the
 *       local streaming session down. Works regardless of current
 *       client state - this is the only way for the device to close a
 *       particular session_id unilaterally.
 *
 * Commands are case-sensitive, terminated by CR / LF, and may be
 * prefixed with whitespace. Anything else on the line is ignored but
 * echoed back for debugging.
 * ----------------------------------------------------------------- */

#if 0
typedef struct {
    bool     listeners_inited;
    bool     active;          /* true between CONV_START and CONV_STOP */
    uint64_t seq;
    uint32_t uplink_chunk_bytes;
    uint32_t sent_total;
    uint32_t audio_frames_played;
} app_console_conv_t;

static app_console_conv_t s_console_conv;

/*
 * Async EndConversation dispatch.
 *
 * Treat ESP:CONV_STOP:<sid> as a fire-and-forget command from the
 * UART reader's point of view: parsing must return immediately, so
 * that a subsequent CONV_START (or another CONV_STOP) is never
 * blocked by the ~hundreds-of-ms TCP+h2 round-trip send_end_rpc
 * needs. A dedicated worker task drains a session-id queue and
 * issues the actual unary RPC.
 *
 * Queue depth 4 is intentionally tiny: if the user somehow spams
 * STOP faster than we can complete the RPC, we prefer to drop the
 * overflow (and log it) rather than grow an unbounded backlog.
 */
#define APP_END_RPC_QUEUE_DEPTH   4
#define APP_END_RPC_SID_MAX       72  /* room for 64-hex + prefix + NUL */
#define APP_END_RPC_STACK_WORDS   4096
#define APP_STACK_WATERMARK_WARN_WORDS 256U

typedef struct {
    char session_id[APP_END_RPC_SID_MAX];
} app_end_rpc_req_t;

static QueueHandle_t s_end_rpc_queue;
static StaticQueue_t s_end_rpc_queue_struct;
static uint8_t s_end_rpc_queue_storage[
    APP_END_RPC_QUEUE_DEPTH * sizeof(app_end_rpc_req_t)
];
static StaticTask_t s_end_rpc_tcb;
static StackType_t s_end_rpc_stack[APP_END_RPC_STACK_WORDS];
static TaskHandle_t s_app_task_handle;
static TaskHandle_t s_console_task_handle;

static void app_stack_guard_tick(uint32_t heartbeat_seq)
{
    if ((heartbeat_seq % 5U) != 0U) {
        return;
    }

    struct {
        const char *name;
        TaskHandle_t handle;
    } tasks[] = {
        { "collar_app", s_app_task_handle },
        { "app_console", s_console_task_handle },
    };

    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        if (tasks[i].handle == NULL) {
            continue;
        }
        UBaseType_t wm = uxTaskGetStackHighWaterMark(tasks[i].handle);
        if (wm <= APP_STACK_WATERMARK_WARN_WORDS) {
            ESP_LOGW(TAG, "stack-guard: task=%s low watermark=%u words",
                     tasks[i].name, (unsigned)wm);
        }
    }
}

static void app_end_rpc_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "end-rpc worker ready");
    for (;;) {
        app_end_rpc_req_t req;
        if (xQueueReceive(s_end_rpc_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "end-rpc worker: dispatch session_id='%s'", req.session_id);
        esp_err_t rv = conversation_client_send_end_rpc(req.session_id);
        ESP_LOGI(TAG, "end-rpc worker: EndConversation RPC result: %s",
                 rv == ESP_OK ? "OK" : "FAIL");
    }
}


static void app_console_on_audio_output(const uint8_t *pcm, size_t len,
                                        int64_t seq, void *arg)
{
    (void)arg; (void)seq;
    if (!pcm || !len) return;
    if (!bsp_speaker_is_ready()) return;
    size_t written = 0U;
    esp_err_t ret = bsp_speaker_write(pcm, len, &written, 60U);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "console: speaker write failed: %s", esp_err_to_name(ret));
        return;
    }
    s_console_conv.audio_frames_played++;
}

static void app_console_on_audio_complete(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "console: DL audio_complete (played=%u frames)",
             (unsigned)s_console_conv.audio_frames_played);
}

static void app_console_on_audio_start(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "console: DL audio_start (server is speaking)");
}

static void app_console_on_session_start(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "console: session_start (first server event)");
}

static void app_console_on_error(const char *code, const char *message, void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "console: server ErrorEvent code='%s' msg='%s'",
             code ? code : "-", message ? message : "-");
}

static void app_console_ensure_listeners(void)
{
    if (s_console_conv.listeners_inited) return;
    conversation_client_set_audio_output_listener(app_console_on_audio_output, NULL);
    conversation_client_set_audio_complete_listener(app_console_on_audio_complete, NULL);
    conversation_client_set_audio_start_listener(app_console_on_audio_start, NULL);
    conversation_client_set_session_start_listener(app_console_on_session_start, NULL);
    conversation_client_set_error_listener(app_console_on_error, NULL);

    const uint32_t rate = CONFIG_COLLAR_CONV_AUDIO_SAMPLE_RATE;
    const uint32_t ch   = CONFIG_COLLAR_CONV_AUDIO_CHANNELS;
    const uint32_t bps  = CONFIG_COLLAR_CONV_AUDIO_BIT_DEPTH / 8U;
    const uint32_t chunk_ms = APP_CONV_UPLINK_CHUNK_MS;
    s_console_conv.uplink_chunk_bytes = (rate * ch * bps * chunk_ms) / 1000U;
    s_console_conv.listeners_inited = true;
    ESP_LOGI(TAG, "console: listeners registered, uplink chunk=%u B (%u ms)",
             (unsigned)s_console_conv.uplink_chunk_bytes, (unsigned)chunk_ms);
}

/*
 * Dedicated mic -> gRPC uplink pump task.
 *
 * Runs forever at higher priority than the main collar_app loop. When
 * s_console_conv.active is true, it blocks on bsp_microphone_read() for a
 * full uplink chunk and then calls conversation_client_send_audio().
 * When inactive, it simply drains whatever the mic driver has buffered so
 * the StreamBuffer does not overflow, and sleeps 50 ms. This guarantees the
 * server sees a continuous PCM stream for as long as the CONV_START
 * session is alive.
 */
static void app_console_mic_uplink_task(void *arg)
{
    (void)arg;
    static uint8_t scratch[APP_MIC_TEST_MAX_CHUNK_BYTES];
    static uint8_t drain[APP_MIC_TEST_MAX_CHUNK_BYTES];

    /* Wait until listeners init populates uplink_chunk_bytes. */
    while (!s_console_conv.listeners_inited || s_console_conv.uplink_chunk_bytes == 0U) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const size_t chunk = s_console_conv.uplink_chunk_bytes > sizeof(scratch)
                         ? sizeof(scratch)
                         : s_console_conv.uplink_chunk_bytes;
    ESP_LOGI(TAG, "mic-uplink task ready: chunk=%u B", (unsigned)chunk);

    uint32_t no_stream_log_seq = 0U;
    uint32_t backpressure_log = 0U;
    for (;;) {
        if (!bsp_microphone_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!s_console_conv.active) {
            /* Session idle: drain any buffered PCM so we don't overflow. */
            size_t got = 0U;
            (void)bsp_microphone_read(drain, sizeof(drain), &got, 0U);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Active session: block up to 2x frame for a full uplink chunk. */
        size_t got = 0U;
        esp_err_t ret = bsp_microphone_read(
            scratch, chunk, &got, (uint32_t)(APP_CONV_UPLINK_CHUNK_MS * 2U));
        if (ret != ESP_OK || got < chunk) {
            /* Not enough yet; try again shortly. */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (!conversation_client_stream_writable()) {
            /* Stream not armed yet (CONNECTING, or just dropped). Keep
             * draining mic so it doesn't back up, and retry. */
            if ((no_stream_log_seq++ % 50U) == 0U) {
                ESP_LOGI(TAG, "mic-uplink waiting: stream not writable, state=%s",
                         conversation_client_state_str(
                             conversation_client_get_state()));
            }
            continue;
        }

        s_console_conv.seq++;
        esp_err_t tx = conversation_client_send_audio(
            scratch, got, s_console_conv.seq);
        if (tx == ESP_OK) {
            s_console_conv.sent_total++;
            if (s_console_conv.sent_total == 1U ||
                (s_console_conv.sent_total % 50U) == 0U) {
                ESP_LOGI(TAG, "mic-uplink: sent=%u chunks (seq=%llu, %ums each)",
                         (unsigned)s_console_conv.sent_total,
                         (unsigned long long)s_console_conv.seq,
                         (unsigned)APP_CONV_UPLINK_CHUNK_MS);
            }
        } else if (tx == ESP_ERR_INVALID_STATE) {
            /* Stream closed between the check and the send. Loop. */
            continue;
        } else if (tx == ESP_ERR_NO_MEM) {
            /* Real-time mode on constrained MCU: do at most a short retry,
             * then drop this chunk and keep draining microphone stream.
             * This prevents 100ms+ stalls that cause PCM overflow bursts. */
            if ((++backpressure_log % 50U) == 1U) {
                ESP_LOGW(TAG, "mic-uplink: backpressure/no-mem, retrying same chunk");
            }
            uint32_t retry = 0U;
            while (retry < APP_UPLINK_BACKPRESSURE_MAX_RETRY &&
                   s_console_conv.active && conversation_client_stream_writable()) {
                vTaskDelay(app_delay_ticks(APP_UPLINK_BACKPRESSURE_RETRY_MS));
                tx = conversation_client_send_audio(scratch, got, s_console_conv.seq);
                if (tx == ESP_OK) {
                    s_console_conv.sent_total++;
                    if (s_console_conv.sent_total == 1U ||
                        (s_console_conv.sent_total % 50U) == 0U) {
                        ESP_LOGI(TAG, "mic-uplink: sent=%u chunks (seq=%llu, %ums each)",
                                 (unsigned)s_console_conv.sent_total,
                                 (unsigned long long)s_console_conv.seq,
                                 (unsigned)APP_CONV_UPLINK_CHUNK_MS);
                    }
                    break;
                }
                if (tx != ESP_ERR_NO_MEM) {
                    break;
                }
                retry++;
            }
            if (tx == ESP_ERR_NO_MEM) {
                /* Controlled drop under sustained congestion. */
                static uint32_t s_bp_drop_count = 0U;
                s_bp_drop_count++;
                if (s_bp_drop_count <= 3U || (s_bp_drop_count % 50U) == 0U) {
                    ESP_LOGW(TAG, "mic-uplink: drop one chunk after %u retries (total=%lu)",
                             (unsigned)APP_UPLINK_BACKPRESSURE_MAX_RETRY,
                             (unsigned long)s_bp_drop_count);
                }
            }
        } else {
            ESP_LOGW(TAG, "mic-uplink: send_audio err=0x%x (len=%u)",
                     tx, (unsigned)got);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

    }
}


static void app_console_drain_mic_stream(void)
{
    static uint8_t drain[APP_MIC_TEST_MAX_CHUNK_BYTES];
    if (!bsp_microphone_is_ready()) return;
    size_t total = 0U;
    for (int i = 0; i < 32; ++i) {
        size_t got = 0U;
        esp_err_t ret = bsp_microphone_read(drain, sizeof(drain), &got, 0U);
        if (ret != ESP_OK || got == 0U) break;
        total += got;
    }
    if (total > 0U) {
        ESP_LOGI(TAG, "console: drained %u B of stale mic PCM", (unsigned)total);
    }
}

static void app_console_handle_start(void)
{
    app_console_ensure_listeners();
    conversation_state_t st = conversation_client_get_state();
    if (st != CONVERSATION_STATE_IDLE) {
        ESP_LOGW(TAG, "console: CONV_START ignored, current state=%s",
                 conversation_client_state_str(st));
        s_console_conv.active = true;  /* keep pumping mic if already running */
        return;
    }
    s_console_conv.seq = 0;
    s_console_conv.sent_total = 0;
    s_console_conv.audio_frames_played = 0;
    ESP_LOGI(TAG, "console: >>> CONV_START received, opening new conversation");
    (void)wifi_service_set_realtime_mode(true);

    /*
     * Memory handoff for ESP32-C3:
     * StreamConversation runs on a manual h2c transport and still needs
     * contiguous heap headroom for socket/queue/buffer bring-up. The idle
     * mic self-test path may keep Speex denoise state resident (large
     * dynamic allocations). Drop denoise right before CONV_START.
     */
    const uint32_t heap_before = (uint32_t)esp_get_free_heap_size();
    const uint32_t largest_before =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (audio_denoise_is_ready()) {
        audio_denoise_deinit();
        ESP_LOGI(TAG, "console: pre-CONV_START released denoise "
                      "(free_heap %u->%u, largest %u->%u)",
                 (unsigned)heap_before,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)largest_before,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }

    /* Flush any pre-roll PCM captured while we were idle so the pump
     * starts at "real now" and does not burst stale audio into the
     * gRPC uplink queue (kept intentionally shallow for low latency). */
    app_console_drain_mic_stream();
    esp_err_t err = conversation_client_start_conversation(NULL);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console: start_conversation err=0x%x", err);
        (void)wifi_service_set_realtime_mode(false);
        return;
    }
    err = conversation_client_start_talking();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console: start_talking err=0x%x", err);
        /* leave streaming up to the client worker - it will transition
         * state once CONNECTING resolves; feed loop will then start
         * delivering audio. */
    }
    s_console_conv.active = true;
}

static void app_console_handle_stop(const char *session_id)
{
    if (!session_id || session_id[0] == '\0') {
        ESP_LOGW(TAG, "console: CONV_STOP requires <session_id>");
        return;
    }
    ESP_LOGI(TAG, "console: >>> CONV_STOP session_id='%s' (sent=%u, played=%u)",
             session_id,
             (unsigned)s_console_conv.sent_total,
             (unsigned)s_console_conv.audio_frames_played);

    /* Stop feeding mic immediately. */
    s_console_conv.active = false;
    (void)wifi_service_set_realtime_mode(false);

    /* Tear down our local streaming session first (half-close). */
    if (conversation_client_get_state() != CONVERSATION_STATE_IDLE) {
        (void)conversation_client_end_conversation();
    }

    /*
     * Fire unary EndConversation RPC asynchronously: dispatch via queue
     * to the end-rpc worker so this UART-reader context returns in
     * microseconds. Each of CONV_START / CONV_STOP is thus a true
     * independent command from the dispatcher's point of view - a
     * second command never has to wait for the previous RPC's
     * TCP+h2 round-trip to finish.
     */
    if (s_end_rpc_queue == NULL) {
        /* Worker didn't come up - fall back to sync call so we don't
         * silently drop the request. */
        esp_err_t rv = conversation_client_send_end_rpc(session_id);
        ESP_LOGI(TAG, "console: EndConversation RPC result (sync fallback): %s",
                 rv == ESP_OK ? "OK" : "FAIL");
        return;
    }
    app_end_rpc_req_t req = {0};
    strlcpy(req.session_id, session_id, sizeof(req.session_id));
    if (xQueueSend(s_end_rpc_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "console: end-rpc queue full, dropping CONV_STOP sid='%s'",
                 session_id);
        return;
    }
    ESP_LOGI(TAG, "console: CONV_STOP dispatched to end-rpc worker (async)");
}
#endif


static void app_console_process_line(char *line)
{
    dialog_orchestrator_process_line(line);
}

static void app_console_task(void *arg)
{
    (void)arg;
    /* UART0 is already brought up by esp-idf's console driver
     * (USB-JTAG or plain UART depending on target). We just need
     * line-mode, no echo, with a modest RX buffer. */
    const uart_port_t port = UART_NUM_0;
    /* Make sure driver is installed (idempotent: returns INVALID_STATE
     * if already installed, which we ignore). */
    (void)uart_driver_install(port, 256, 0, 0, NULL, 0);

    char line[96];
    size_t pos = 0U;
    ESP_LOGI(TAG, "console: command reader ready (ESP:CONV_START / ESP:CONV_STOP:<sid>)");
    for (;;) {
        uint8_t ch = 0;
        int r = uart_read_bytes(port, &ch, 1, pdMS_TO_TICKS(100));
        if (r <= 0) continue;
        if (ch == '\r' || ch == '\n') {
            if (pos == 0U) continue;
            line[pos] = '\0';
            app_console_process_line(line);
            pos = 0U;
            continue;
        }
        if (pos < sizeof(line) - 1U) {
            line[pos++] = (char)ch;
        } else {
            ESP_LOGW(TAG, "console: line overflow, dropping");
            pos = 0U;
        }
    }
}

static void collar_app_task(void *arg)


{
    (void)arg;

    uint32_t heartbeat_seq = 0;
    int64_t next_heartbeat_us = esp_timer_get_time();

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        app_audio_mode_tick(now_us);


#if CONFIG_COLLAR_SPEAKER_TEST_ENABLE
        if (s_audio_runtime.mode == APP_AUDIO_MODE_BOOT_CHIME) {
            app_speaker_test_tick();
        }
#endif
#if CONFIG_COLLAR_MICROPHONE_TEST_ENABLE
        if (s_audio_runtime.mode == APP_AUDIO_MODE_MIC &&
            conversation_client_get_state() == CONVERSATION_STATE_IDLE &&
            !dialog_orchestrator_is_active()) {
            app_mic_test_tick();
        }
#endif
        /* Mic uplink while a conversation is active is handled by the
         * dialog_uplink module task, not here. */




        app_audio_log_status_if_due(now_us);

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
    /* Console command reader (ESP:CONV_START / ESP:CONV_STOP:<sid>) */
    TaskHandle_t console_th = xTaskCreateStatic(
        app_console_task, "app_console",
        APP_CONSOLE_STACK_WORDS, NULL, 3,
        s_console_stack, &s_console_tcb);
    if (console_th == NULL) {
        ESP_LOGW(TAG, "console task create failed");
    }

    esp_err_t orch_ret = dialog_orchestrator_start();
    if (orch_ret != ESP_OK) {
        ESP_LOGW(TAG, "dialog orchestrator start failed: %s", esp_err_to_name(orch_ret));
    }

    s_app_started = true;


    kernel_trace_boot("app manager started");
    return ESP_OK;
}
