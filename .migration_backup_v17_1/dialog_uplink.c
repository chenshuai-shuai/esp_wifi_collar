#include "dialog_uplink.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/microphone_input.h"
#include "conversation/conversation_client.h"

#define DIALOG_UL_STACK_WORDS            6144
#define DIALOG_UL_TASK_PRIORITY          5
#define DIALOG_UL_CHUNK_MS               20U
#define DIALOG_UL_RETRY_MS               5U
#define DIALOG_UL_SCRATCH_MAX_BYTES      8192U
#define DIALOG_UL_START_HOLDOFF_MS       5000U
#define DIALOG_UL_BP_ABORT_RETRIES       200U

static const char *TAG = "dlg_ul";

static StaticTask_t s_ul_tcb;
static StackType_t s_ul_stack[DIALOG_UL_STACK_WORDS];
static TaskHandle_t s_ul_task;

static volatile bool s_ul_active;
static int64_t s_start_holdoff_until_us;
static bool s_holdoff_logged;
static dialog_uplink_stats_t s_stats;
static uint32_t s_drain_log_bytes;
static uint32_t s_drain_log_calls;
static int64_t s_drain_log_last_us;

static uint32_t dialog_calc_chunk_bytes(void)
{
    const uint32_t rate = CONFIG_COLLAR_CONV_AUDIO_SAMPLE_RATE;
    const uint32_t ch = CONFIG_COLLAR_CONV_AUDIO_CHANNELS;
    const uint32_t bps = CONFIG_COLLAR_CONV_AUDIO_BIT_DEPTH / 8U;
    return (rate * ch * bps * DIALOG_UL_CHUNK_MS) / 1000U;
}

void dialog_uplink_drain_stale(void)
{
    static uint8_t drain[DIALOG_UL_SCRATCH_MAX_BYTES];

    if (!bsp_microphone_is_ready()) {
        return;
    }

    size_t total = 0U;
    for (int i = 0; i < 32; ++i) {
        size_t got = 0U;
        esp_err_t ret = bsp_microphone_read(drain, sizeof(drain), &got, 0U);
        if (ret != ESP_OK || got == 0U) {
            break;
        }
        total += got;
    }

    if (total > 0U) {
        const int64_t now_us = esp_timer_get_time();
        s_drain_log_bytes += (uint32_t)total;
        s_drain_log_calls += 1U;
        if ((now_us - s_drain_log_last_us) >= 2000000LL) {
            ESP_LOGI(TAG, "drained stale mic PCM: calls=%u bytes=%u (2s aggregate)",
                     (unsigned)s_drain_log_calls,
                     (unsigned)s_drain_log_bytes);
            s_drain_log_calls = 0U;
            s_drain_log_bytes = 0U;
            s_drain_log_last_us = now_us;
        }
    }
}

static void dialog_uplink_task(void *arg)
{
    (void)arg;

    static uint8_t scratch[DIALOG_UL_SCRATCH_MAX_BYTES];
    const size_t chunk = s_stats.chunk_bytes > sizeof(scratch)
                             ? sizeof(scratch)
                             : s_stats.chunk_bytes;
    size_t filled = 0U;

    ESP_LOGI(TAG, "uplink task ready: chunk=%u B (%u ms)",
             (unsigned)chunk,
             (unsigned)DIALOG_UL_CHUNK_MS);

    uint32_t no_stream_log_seq = 0U;
    uint32_t bp_log = 0U;
    uint32_t bp_streak = 0U;
    uint32_t read_log_seq = 0U;
    uint32_t fill_log_seq = 0U;
    uint32_t tx_log_seq = 0U;
    uint32_t audio_probe_seq = 0U;

    for (;;) {
        if (!bsp_microphone_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!s_ul_active) {
            dialog_uplink_drain_stale();
            filled = 0U;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* If conversation side auto-aborted (send-fail/backpressure), align
         * uplink activity state with session state and stop pumping. */
        if (!conversation_client_session_active()) {
            if (s_ul_active) {
                ESP_LOGW(TAG, "session no longer active -> uplink auto stop");
            }
            s_ul_active = false;
            filled = 0U;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us < s_start_holdoff_until_us) {
            if (!s_holdoff_logged) {
                const int64_t left_ms = (s_start_holdoff_until_us - now_us) / 1000LL;
                ESP_LOGI(TAG, "uplink holdoff: wait %lld ms before first upload",
                         (long long)left_ms);
                s_holdoff_logged = true;
            }
            dialog_uplink_drain_stale();
            filled = 0U;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (filled < chunk) {
            size_t got = 0U;
            const size_t need = chunk - filled;
            esp_err_t ret = bsp_microphone_read(
                scratch + filled, need, &got, (uint32_t)(DIALOG_UL_CHUNK_MS));
            if (ret != ESP_OK || got == 0U) {
                if ((read_log_seq++ % 100U) == 0U) {
                    ESP_LOGI(TAG, "mic read pending: ret=0x%x got=%u filled=%u need=%u",
                             ret, (unsigned)got, (unsigned)filled, (unsigned)need);
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            filled += got;
            if (filled < chunk) {
                if ((fill_log_seq++ % 50U) == 0U) {
                    ESP_LOGI(TAG, "mic fill: %u/%u B", (unsigned)filled, (unsigned)chunk);
                }
                continue;
            }
        }

        if (!conversation_client_session_active()) {
            if ((no_stream_log_seq++ % 50U) == 0U) {
                ESP_LOGI(TAG,
                         "waiting active session, state=%s",
                         conversation_client_state_str(conversation_client_get_state()));
            }
            filled = 0U;
            continue;
        }

        if (!conversation_client_uplink_can_accept()) {
            if ((bp_log++ % 50U) == 0U) {
                ESP_LOGW(TAG, "uplink queue/backpressure: drop local chunk seq=%llu",
                         (unsigned long long)(s_stats.seq + 1U));
            }
            filled = 0U;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        uint64_t seq_try = s_stats.seq + 1U;
        esp_err_t tx = ESP_FAIL;
        while (s_ul_active) {
            if (!conversation_client_session_active()) {
                tx = ESP_ERR_INVALID_STATE;
                break;
            }

            tx = conversation_client_send_audio(scratch, chunk, seq_try);
            if (tx == ESP_OK) {
                s_stats.seq = seq_try;
                s_stats.sent_total++;
                bp_streak = 0U;
                filled = 0U;
                if ((tx_log_seq++ % 25U) == 0U) {
                    ESP_LOGI(TAG, "uplink tx ok: sent_total=%u seq=%llu chunk=%u",
                             (unsigned)s_stats.sent_total,
                             (unsigned long long)s_stats.seq,
                             (unsigned)chunk);
                }
                if ((audio_probe_seq++ % 25U) == 0U) {
                    const int16_t *pcm16 = (const int16_t *)scratch;
                    const size_t samples = chunk / sizeof(int16_t);
                    uint32_t peak = 0U;
                    uint32_t zero = 0U;
                    uint64_t abs_sum = 0U;
                    for (size_t i = 0; i < samples; ++i) {
                        int32_t v = pcm16[i];
                        if (v == 0) zero++;
                        if (v < 0) v = -v;
                        if ((uint32_t)v > peak) peak = (uint32_t)v;
                        abs_sum += (uint32_t)v;
                    }
                    const uint32_t mean_abs = samples > 0 ? (uint32_t)(abs_sum / samples) : 0U;
                    const uint32_t zero_permille = samples > 0 ? (uint32_t)((zero * 1000U) / samples) : 0U;
                    ESP_LOGI(TAG,
                             "pcm probe: seq=%llu samples=%u mean_abs=%u peak=%u zero=%u.%u%%",
                             (unsigned long long)s_stats.seq,
                             (unsigned)samples,
                             (unsigned)mean_abs,
                             (unsigned)peak,
                             (unsigned)(zero_permille / 10U),
                             (unsigned)(zero_permille % 10U));
                }
                if (s_stats.sent_total == 1U || (s_stats.sent_total % 50U) == 0U) {
                    ESP_LOGI(TAG,
                             "sent=%u chunks (seq=%llu)",
                             (unsigned)s_stats.sent_total,
                             (unsigned long long)s_stats.seq);
                }
                /* Prevent task watchdog starvation on single-core C3:
                 * yield one tick after each successful uplink packet so IDLE can run. */
                vTaskDelay(pdMS_TO_TICKS(1));
                break;
            }

            if (tx == ESP_ERR_NO_MEM) {
                bp_streak++;
                if ((++bp_log % 50U) == 1U) {
                    ESP_LOGW(TAG, "backpressure/no-mem: hold current chunk seq=%llu",
                             (unsigned long long)seq_try);
                }
                if (bp_streak >= DIALOG_UL_BP_ABORT_RETRIES) {
                    ESP_LOGE(TAG,
                             "backpressure too long: retries=%u seq=%llu -> auto abort session",
                             (unsigned)bp_streak,
                             (unsigned long long)seq_try);
                    (void)conversation_client_abort_session("uplink-backpressure", ESP_ERR_NO_MEM);
                    s_ul_active = false;
                    filled = 0U;
                    bp_streak = 0U;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(DIALOG_UL_RETRY_MS));
                continue;
            }

            bp_streak = 0U;
            ESP_LOGW(TAG, "send_audio err=0x%x len=%u seq=%llu",
                     tx, (unsigned)chunk, (unsigned long long)seq_try);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t dialog_uplink_start(void)
{
    if (s_ul_task != NULL) {
        return ESP_OK;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.chunk_bytes = dialog_calc_chunk_bytes();

    s_ul_task = xTaskCreateStatic(
        dialog_uplink_task,
        "dlg_ul",
        DIALOG_UL_STACK_WORDS,
        NULL,
        DIALOG_UL_TASK_PRIORITY,
        s_ul_stack,
        &s_ul_tcb);

    if (s_ul_task == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void dialog_uplink_set_active(bool active)
{
    const bool was_active = s_ul_active;
    s_ul_active = active;
    if (active && !was_active) {
        s_start_holdoff_until_us =
            esp_timer_get_time() + ((int64_t)DIALOG_UL_START_HOLDOFF_MS * 1000LL);
        s_holdoff_logged = false;
    } else if (!active) {
        s_start_holdoff_until_us = 0LL;
        s_holdoff_logged = false;
    }
}

void dialog_uplink_resume_now(void)
{
    s_ul_active = true;
    s_start_holdoff_until_us = 0LL;
    s_holdoff_logged = false;
}

bool dialog_uplink_is_active(void)
{
    return s_ul_active;
}

void dialog_uplink_reset_turn(void)
{
    /* Android GrpcAudioClient direct mic paths start talkSeq at 1 and then
     * send talkSeq++. Keep ESP uplink sequence numbering aligned with that. */
    s_stats.seq = 0U;
    s_stats.sent_total = 0U;
    s_stats.dropped_total = 0U;
}

void dialog_uplink_get_stats(dialog_uplink_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    *stats = s_stats;
}

uint32_t dialog_uplink_chunk_bytes(void)
{
    return s_stats.chunk_bytes;
}
