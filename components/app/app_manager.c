#include <stdint.h>
#include <string.h>

#include "app/app_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "kernel/kernel_msgbus.h"
#include "kernel/kernel_trace.h"
#include "services/conversation_service.h"

#define APP_MANAGER_STACK_WORDS        4096
#define APP_MANAGER_PRIORITY           8
#define APP_MANAGER_CORE               tskNO_AFFINITY
#define APP_HEARTBEAT_PERIOD_MS        1000
#define APP_LOOP_PERIOD_MS             20
#define APP_SELF_TEST_MAX_CHUNK_BYTES  2048

static const char *TAG = "app_mgr";

static StaticTask_t s_app_tcb;
static StackType_t s_app_stack[APP_MANAGER_STACK_WORDS];
static bool s_app_started;

#if CONFIG_COLLAR_CONVERSATION_SELF_TEST_ENABLE
typedef struct {
    bool session_requested;
    bool completion_logged;
    uint32_t sent_chunks;
    uint32_t target_chunks;
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
    const uint32_t bytes_per_sample = (CONFIG_COLLAR_CONVERSATION_AUDIO_BIT_DEPTH + 7U) / 8U;
    const uint32_t channels = CONFIG_COLLAR_CONVERSATION_AUDIO_CHANNELS;
    const uint32_t bytes_per_frame = channels * bytes_per_sample;
    const size_t frames = (bytes_per_frame == 0U) ? 0U : (len / bytes_per_frame);
    uint32_t sample_cursor = (uint32_t)(seq * frames);

    memset(buffer, 0, len);

    if (bytes_per_sample != 2U || bytes_per_frame == 0U) {
        for (size_t i = 0; i < len; ++i) {
            buffer[i] = (uint8_t)((sample_cursor + i) & 0xffU);
        }
        return;
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        const uint32_t phase = (sample_cursor + (uint32_t)frame) % 96U;
        const int32_t rising = ((int32_t)phase * 1365) - 32768;
        const int32_t falling = ((int32_t)(95U - phase) * 1365) - 32768;
        const int16_t sample = (int16_t)((phase < 48U) ? rising : falling);

        for (uint32_t channel = 0; channel < channels; ++channel) {
            const size_t offset = (frame * bytes_per_frame) + (channel * bytes_per_sample);
            buffer[offset] = (uint8_t)(sample & 0xff);
            buffer[offset + 1U] = (uint8_t)((sample >> 8) & 0xff);
        }
    }
}

static void app_self_test_reset(int64_t now_us, bool keep_delay)
{
    memset(&s_self_test, 0, sizeof(s_self_test));
    s_self_test.chunk_bytes = (uint32_t)app_self_test_chunk_bytes();
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
        s_self_test.next_send_us = now_us;
        ESP_LOGI(TAG,
                 "Conversation self-test session requested: id=%s chunk=%luB count=%lu total=%dms",
                 s_self_test.session_id,
                 (unsigned long)s_self_test.chunk_bytes,
                 (unsigned long)s_self_test.target_chunks,
                 CONFIG_COLLAR_CONVERSATION_SELF_TEST_TOTAL_MS);
        return;
    }

    if (!conversation_service_stream_writable()) {
        return;
    }

    if (s_self_test.sent_chunks >= s_self_test.target_chunks) {
        if (!s_self_test.completion_logged) {
            ESP_LOGI(TAG,
                     "Conversation self-test upload complete: session=%s chunks=%lu bytes=%lu",
                     s_self_test.session_id,
                     (unsigned long)s_self_test.sent_chunks,
                     (unsigned long)(s_self_test.sent_chunks * s_self_test.chunk_bytes));
            s_self_test.completion_logged = true;
            s_self_test.restart_after_us =
                now_us + ((int64_t)CONFIG_COLLAR_CONVERSATION_SELF_TEST_REPEAT_DELAY_SEC * 1000000LL);
        }
        return;
    }

    if (now_us < s_self_test.next_send_us) {
        return;
    }

    app_self_test_fill_pcm(pcm_buffer, s_self_test.chunk_bytes, s_self_test.next_seq);
    esp_err_t ret = conversation_service_send_audio(
        pcm_buffer, s_self_test.chunk_bytes, s_self_test.next_seq);
    if (ret == ESP_OK) {
        s_self_test.sent_chunks++;
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
