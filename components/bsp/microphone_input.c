#include "bsp/microphone_input.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "mic";

// Experimental raw PDM capture on ESP32-C3:
// 1. SPI master generates the microphone clock and samples the shared 1-bit bus.
// 2. In mono mode we sample a single edge slot.
// 3. In stereo mode we capture one continuous shared-data bitstream at 2x clock
//    and de-multiplex alternating slots into left/right PCM in software.
#if CONFIG_COLLAR_MICROPHONE_ENABLE
#define BSP_MIC_SPI_HOST                SPI2_HOST
#define BSP_MIC_RAW_BYTES               2048U
#define BSP_MIC_STREAM_BUFFER_MS        250U
#define BSP_MIC_STREAM_BUFFER_MAX_BYTES (48U * 1024U)
#define BSP_MIC_MIN_DECIMATION          64U
#define BSP_MIC_PCM_SAMPLES_PER_BLOCK   ((BSP_MIC_RAW_BYTES * 8U) / BSP_MIC_MIN_DECIMATION)
#define BSP_MIC_I2S_SLOT_BITS           16U
#define BSP_MIC_I2S_SLOT_COUNT          2U
#define BSP_MIC_BASE_PDM_CLOCK_HZ       1500000U
#define BSP_MIC_MAX_PDM_CLOCK_HZ        3300000U
#define BSP_MIC_WARMUP_MS               25U
#define BSP_MIC_TASK_STACK_WORDS        6144U
#define BSP_MIC_TASK_PRIORITY           5U
#define BSP_MIC_DC_ALPHA_Q15            32604
#define BSP_MIC_OVERFLOW_LOG_PERIOD_US  1000000LL
#define BSP_MIC_DIAG_LOG_PERIOD_US      3000000LL
#define BSP_MIC_YIELD_EVERY_LOOPS       8U
#define BSP_MIC_PREVIEW_BYTES           8U
#define BSP_MIC_PREVIEW_SAMPLES         8U
#define BSP_MIC_CAPTURE_LANE_COUNT      2U
#define BSP_MIC_MONO_LANE_INDEX         0U
#define BSP_MIC_LEFT_LANE_INDEX         0U
#define BSP_MIC_RIGHT_LANE_INDEX        1U
#define BSP_MIC_STEREO_SHARED_SPI_INDEX 0U

typedef struct {
    uint32_t warmup_samples_remaining;
    uint16_t pdm_bit_count;
    int32_t pdm_accum;
    int32_t dc_prev_x;
    int32_t dc_prev_y;
    uint32_t diag_raw_blocks;
    uint32_t diag_raw_bits;
    uint32_t diag_raw_bytes;
    uint32_t diag_pdm_ones;
    uint32_t diag_pdm_edges;
    uint32_t diag_pcm_samples;
    uint32_t diag_pcm_peak;
    uint64_t diag_pcm_abs_sum;
    uint8_t diag_prev_bit;
    bool diag_prev_bit_valid;
} microphone_lane_state_t;

typedef struct {
    spi_device_handle_t spi[BSP_MIC_CAPTURE_LANE_COUNT];
    i2s_chan_handle_t i2s_rx;
    StreamBufferHandle_t pcm_stream;
    TaskHandle_t task;
    uint8_t *tx_dummy;
    uint8_t *rx_dma[BSP_MIC_CAPTURE_LANE_COUNT];
    uint32_t sample_rate_hz;
    uint32_t capture_clock_hz;
    uint32_t decimation;
    uint8_t channels;
    uint32_t overflow_events;
    uint32_t overflow_bytes;
    int64_t last_overflow_log_us;
    int64_t last_diag_log_us;
    int64_t last_capture_warn_log_us;
    bool task_running;
    uint32_t diag_capture_loops;
    uint32_t diag_i2s_reads;
    uint32_t diag_i2s_zero_reads;
    uint32_t diag_i2s_short_reads;
    uint32_t diag_i2s_odd_reads;
    uint32_t diag_raw_truncated_blocks;
    uint32_t diag_pcm_zero_blocks;
    uint32_t diag_i2s_bytes;
    uint32_t diag_last_i2s_bytes;
    uint32_t diag_last_raw_bytes;
    uint32_t diag_last_pcm_samples;
    uint8_t diag_last_i2s_preview[BSP_MIC_PREVIEW_BYTES];
    uint8_t diag_last_raw_preview[BSP_MIC_PREVIEW_BYTES];
    uint32_t diag_last_i2s_preview_len;
    uint32_t diag_last_raw_preview_len;
    int16_t diag_last_pcm_preview[BSP_MIC_PREVIEW_SAMPLES];
    uint32_t diag_last_pcm_preview_len;
    microphone_lane_state_t lane[BSP_MIC_CAPTURE_LANE_COUNT];
    int16_t lane_pcm[BSP_MIC_CAPTURE_LANE_COUNT][BSP_MIC_PCM_SAMPLES_PER_BLOCK];
    int16_t stereo_pcm[BSP_MIC_PCM_SAMPLES_PER_BLOCK * BSP_MIC_CAPTURE_LANE_COUNT];
    uint8_t raw_pdm_bytes[BSP_MIC_RAW_BYTES];
} microphone_state_t;

static microphone_state_t s_mic;
static bool s_microphone_ready;

static inline int16_t mic_clip_s16(int32_t sample)
{
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static uint32_t mic_compute_decimation(uint32_t sample_rate_hz)
{
    uint32_t decimation = (BSP_MIC_BASE_PDM_CLOCK_HZ + sample_rate_hz - 1U) / sample_rate_hz;
    if (decimation < BSP_MIC_MIN_DECIMATION) {
        decimation = BSP_MIC_MIN_DECIMATION;
    }
    return decimation;
}

static size_t mic_stream_buffer_bytes(uint32_t sample_rate_hz, uint8_t channels)
{
    size_t bytes = (size_t)(((uint64_t)sample_rate_hz * channels * sizeof(int16_t) *
                             BSP_MIC_STREAM_BUFFER_MS) / 1000ULL);
    if (bytes < 1024U) {
        bytes = 1024U;
    }
    if (bytes > BSP_MIC_STREAM_BUFFER_MAX_BYTES) {
        bytes = BSP_MIC_STREAM_BUFFER_MAX_BYTES;
    }
    return bytes;
}

static uint32_t mic_i2s_raw_sample_rate_hz(uint32_t capture_clock_hz)
{
    return capture_clock_hz / (BSP_MIC_I2S_SLOT_BITS * BSP_MIC_I2S_SLOT_COUNT);
}

static bool mic_should_route_bit_to_left(size_t bit_index)
{
    return (bit_index & 1U) == 0U;
}

static void mic_reset_lane_state(microphone_lane_state_t *lane, uint32_t sample_rate_hz)
{
    memset(lane, 0, sizeof(*lane));
    lane->warmup_samples_remaining =
        (uint32_t)(((uint64_t)sample_rate_hz * BSP_MIC_WARMUP_MS) / 1000ULL);
}

static size_t mic_pdm_to_pcm_lane(microphone_lane_state_t *lane,
                                  const uint8_t *raw_pdm,
                                  size_t raw_len,
                                  uint32_t decimation,
                                  int16_t *pcm_out,
                                  size_t pcm_capacity)
{
    size_t pcm_count = 0U;

    for (size_t byte_index = 0; byte_index < raw_len; ++byte_index) {
        uint8_t bits = raw_pdm[byte_index];
        for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U) {
            lane->pdm_accum += (bits & mask) ? 1 : -1;
            lane->pdm_bit_count++;
            if (lane->pdm_bit_count != decimation) {
                continue;
            }

            int32_t sample = (lane->pdm_accum * INT16_MAX) / (int32_t)decimation;
            int32_t dc_blocked =
                sample - lane->dc_prev_x + ((BSP_MIC_DC_ALPHA_Q15 * lane->dc_prev_y) >> 15);

            lane->dc_prev_x = sample;
            lane->dc_prev_y = dc_blocked;
            lane->pdm_accum = 0;
            lane->pdm_bit_count = 0U;

            if (lane->warmup_samples_remaining > 0U) {
                lane->warmup_samples_remaining--;
                continue;
            }

            if (pcm_count < pcm_capacity) {
                pcm_out[pcm_count++] = mic_clip_s16(dc_blocked);
            }
        }
    }

    return pcm_count;
}

static void mic_pdm_push_bit(microphone_lane_state_t *lane,
                             uint8_t bit,
                             uint32_t decimation,
                             int16_t *pcm_out,
                             size_t pcm_capacity,
                             size_t *pcm_count)
{
    lane->pdm_accum += bit ? 1 : -1;
    lane->pdm_bit_count++;
    if (lane->pdm_bit_count != decimation) {
        return;
    }

    int32_t sample = (lane->pdm_accum * INT16_MAX) / (int32_t)decimation;
    int32_t dc_blocked =
        sample - lane->dc_prev_x + ((BSP_MIC_DC_ALPHA_Q15 * lane->dc_prev_y) >> 15);

    lane->dc_prev_x = sample;
    lane->dc_prev_y = dc_blocked;
    lane->pdm_accum = 0;
    lane->pdm_bit_count = 0U;

    if (lane->warmup_samples_remaining > 0U) {
        lane->warmup_samples_remaining--;
        return;
    }

    if (*pcm_count < pcm_capacity) {
        pcm_out[(*pcm_count)++] = mic_clip_s16(dc_blocked);
    }
}

static void mic_pdm_interleaved_to_pcm_stereo(const uint8_t *raw_pdm,
                                              size_t raw_len,
                                              uint32_t decimation,
                                              int16_t *left_pcm,
                                              size_t left_capacity,
                                              size_t *left_count,
                                              int16_t *right_pcm,
                                              size_t right_capacity,
                                              size_t *right_count)
{
    size_t bit_index = 0U;

    *left_count = 0U;
    *right_count = 0U;

    for (size_t byte_index = 0; byte_index < raw_len; ++byte_index) {
        uint8_t bits = raw_pdm[byte_index];
        for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U, ++bit_index) {
            const uint8_t bit = (bits & mask) ? 1U : 0U;
            microphone_lane_state_t *lane = mic_should_route_bit_to_left(bit_index) ?
                &s_mic.lane[BSP_MIC_LEFT_LANE_INDEX] :
                &s_mic.lane[BSP_MIC_RIGHT_LANE_INDEX];

            if (lane == &s_mic.lane[BSP_MIC_LEFT_LANE_INDEX]) {
                mic_pdm_push_bit(lane, bit, decimation, left_pcm, left_capacity, left_count);
            } else {
                mic_pdm_push_bit(lane, bit, decimation, right_pcm, right_capacity, right_count);
            }
        }
    }
}

static void mic_accumulate_raw_diag(microphone_lane_state_t *lane, const uint8_t *raw_pdm, size_t raw_len)
{
    lane->diag_raw_blocks++;
    lane->diag_raw_bits += (uint32_t)(raw_len * 8U);
    lane->diag_raw_bytes += (uint32_t)raw_len;

    for (size_t byte_index = 0; byte_index < raw_len; ++byte_index) {
        uint8_t bits = raw_pdm[byte_index];
        lane->diag_pdm_ones += (uint32_t)__builtin_popcount((unsigned int)bits);

        for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U) {
            uint8_t bit = (bits & mask) ? 1U : 0U;
            if (lane->diag_prev_bit_valid && bit != lane->diag_prev_bit) {
                lane->diag_pdm_edges++;
            }
            lane->diag_prev_bit = bit;
            lane->diag_prev_bit_valid = true;
        }
    }
}

static void mic_accumulate_interleaved_raw_diag(const uint8_t *raw_pdm, size_t raw_len)
{
    s_mic.lane[BSP_MIC_LEFT_LANE_INDEX].diag_raw_blocks++;
    s_mic.lane[BSP_MIC_RIGHT_LANE_INDEX].diag_raw_blocks++;

    size_t bit_index = 0U;
    for (size_t byte_index = 0; byte_index < raw_len; ++byte_index) {
        uint8_t bits = raw_pdm[byte_index];
        for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U, ++bit_index) {
            const uint8_t bit = (bits & mask) ? 1U : 0U;
            microphone_lane_state_t *lane = mic_should_route_bit_to_left(bit_index) ?
                &s_mic.lane[BSP_MIC_LEFT_LANE_INDEX] :
                &s_mic.lane[BSP_MIC_RIGHT_LANE_INDEX];

            lane->diag_raw_bits++;
            if (bit != 0U) {
                lane->diag_pdm_ones++;
            }
            if (lane->diag_prev_bit_valid && bit != lane->diag_prev_bit) {
                lane->diag_pdm_edges++;
            }
            lane->diag_prev_bit = bit;
            lane->diag_prev_bit_valid = true;
        }
    }
}

static void mic_accumulate_pcm_diag(microphone_lane_state_t *lane, const int16_t *pcm, size_t pcm_count)
{
    for (size_t i = 0; i < pcm_count; ++i) {
        int32_t sample = pcm[i];
        uint32_t abs_sample = (uint32_t)(sample < 0 ? -sample : sample);
        lane->diag_pcm_abs_sum += abs_sample;
        if (abs_sample > lane->diag_pcm_peak) {
            lane->diag_pcm_peak = abs_sample;
        }
    }
    lane->diag_pcm_samples += (uint32_t)pcm_count;
}

static size_t mic_normalize_i2s_words_to_raw_bytes(const uint8_t *i2s_bytes,
                                                   size_t i2s_len,
                                                   uint8_t *raw_bytes,
                                                   size_t raw_capacity)
{
    size_t normalized = 0U;
    for (size_t i = 0; (i + 1U) < i2s_len && (normalized + 1U) < raw_capacity; i += 2U) {
        raw_bytes[normalized++] = i2s_bytes[i + 1U];
        raw_bytes[normalized++] = i2s_bytes[i];
    }
    return normalized;
}

static void mic_log_mono_flow_diag(const microphone_lane_state_t *lane,
                                   uint32_t ones_permille,
                                   uint32_t edge_permille,
                                   uint32_t pcm_avg_abs)
{
    uint32_t spaces = 0U;
    UBaseType_t stack_words = 0U;
    if (s_mic.pcm_stream != NULL) {
        spaces = (uint32_t)xStreamBufferSpacesAvailable(s_mic.pcm_stream);
    }
    if (s_mic.task != NULL) {
        stack_words = uxTaskGetStackHighWaterMark(s_mic.task);
    }

    ESP_LOGI(TAG,
             "Mic health: backend=i2s-raw slot=%s raw=%uB pcm=%u ones=%u.%u%% edges=%u.%u%% avg=%u peak=%u i2s(last=%uB short=%u zero=%u odd=%u) path(raw_trunc=%u pcm_zero=%u stream_free=%u stack_hwm=%u)",
#ifdef CONFIG_COLLAR_MICROPHONE_LR_HIGH
             "right",
#else
             "left",
#endif
             (unsigned int)lane->diag_raw_bytes,
             (unsigned int)lane->diag_pcm_samples,
             (unsigned int)(ones_permille / 10U),
             (unsigned int)(ones_permille % 10U),
             (unsigned int)(edge_permille / 10U),
             (unsigned int)(edge_permille % 10U),
             (unsigned int)pcm_avg_abs,
             (unsigned int)lane->diag_pcm_peak,
             (unsigned int)s_mic.diag_last_i2s_bytes,
             (unsigned int)s_mic.diag_i2s_short_reads,
             (unsigned int)s_mic.diag_i2s_zero_reads,
             (unsigned int)s_mic.diag_i2s_odd_reads,
             (unsigned int)s_mic.diag_raw_truncated_blocks,
             (unsigned int)s_mic.diag_pcm_zero_blocks,
             (unsigned int)spaces,
             (unsigned int)stack_words);

    (void)lane;
}

static void mic_log_stereo_flow_diag(const microphone_lane_state_t *left,
                                     const microphone_lane_state_t *right)
{
    uint32_t spaces = 0U;
    UBaseType_t stack_words = 0U;
    if (s_mic.pcm_stream != NULL) {
        spaces = (uint32_t)xStreamBufferSpacesAvailable(s_mic.pcm_stream);
    }
    if (s_mic.task != NULL) {
        stack_words = uxTaskGetStackHighWaterMark(s_mic.task);
    }

    ESP_LOGI(TAG,
             "Mic health: backend=spi-pdm stereo raw_clk=%uHz L(avg=%u peak=%u) R(avg=%u peak=%u) stream_free=%u stack_hwm=%u loops=%u",
             (unsigned int)s_mic.capture_clock_hz,
             (unsigned int)(left->diag_pcm_samples == 0U ? 0U : (uint32_t)(left->diag_pcm_abs_sum / left->diag_pcm_samples)),
             (unsigned int)left->diag_pcm_peak,
             (unsigned int)(right->diag_pcm_samples == 0U ? 0U : (uint32_t)(right->diag_pcm_abs_sum / right->diag_pcm_samples)),
             (unsigned int)right->diag_pcm_peak,
             (unsigned int)spaces,
             (unsigned int)stack_words,
             (unsigned int)s_mic.diag_capture_loops);
}

static void mic_log_diag_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if ((now_us - s_mic.last_diag_log_us) < BSP_MIC_DIAG_LOG_PERIOD_US) {
        return;
    }

    if (s_mic.channels > 1U) {
        const microphone_lane_state_t *left = &s_mic.lane[BSP_MIC_LEFT_LANE_INDEX];
        const microphone_lane_state_t *right = &s_mic.lane[BSP_MIC_RIGHT_LANE_INDEX];
        uint32_t left_bits = left->diag_raw_bits;
        uint32_t right_bits = right->diag_raw_bits;
        uint32_t left_ones_permille = left_bits == 0U ? 0U :
            (uint32_t)(((uint64_t)left->diag_pdm_ones * 1000ULL) / left_bits);
        uint32_t right_ones_permille = right_bits == 0U ? 0U :
            (uint32_t)(((uint64_t)right->diag_pdm_ones * 1000ULL) / right_bits);
        uint32_t left_edge_permille = left_bits <= 1U ? 0U :
            (uint32_t)(((uint64_t)left->diag_pdm_edges * 1000ULL) / (uint64_t)(left_bits - 1U));
        uint32_t right_edge_permille = right_bits <= 1U ? 0U :
            (uint32_t)(((uint64_t)right->diag_pdm_edges * 1000ULL) / (uint64_t)(right_bits - 1U));
        uint32_t left_avg_abs = left->diag_pcm_samples == 0U ? 0U :
            (uint32_t)(left->diag_pcm_abs_sum / left->diag_pcm_samples);
        uint32_t right_avg_abs = right->diag_pcm_samples == 0U ? 0U :
            (uint32_t)(right->diag_pcm_abs_sum / right->diag_pcm_samples);

        ESP_LOGI(TAG,
                 "Mic diag: stereo raw_clk=%uHz L(ones=%u.%u%% edges=%u.%u%% avg=%u peak=%u) R(ones=%u.%u%% edges=%u.%u%% avg=%u peak=%u)",
                 (unsigned int)s_mic.capture_clock_hz,
                 (unsigned int)(left_ones_permille / 10U),
                 (unsigned int)(left_ones_permille % 10U),
                 (unsigned int)(left_edge_permille / 10U),
                 (unsigned int)(left_edge_permille % 10U),
                 (unsigned int)left_avg_abs,
                 (unsigned int)left->diag_pcm_peak,
                 (unsigned int)(right_ones_permille / 10U),
                 (unsigned int)(right_ones_permille % 10U),
                 (unsigned int)(right_edge_permille / 10U),
                 (unsigned int)(right_edge_permille % 10U),
                 (unsigned int)right_avg_abs,
                 (unsigned int)right->diag_pcm_peak);
        mic_log_stereo_flow_diag(left, right);
    } else {
        const microphone_lane_state_t *lane = &s_mic.lane[BSP_MIC_MONO_LANE_INDEX];
        uint32_t total_bits = lane->diag_raw_bits;
        uint32_t ones_permille = total_bits == 0U ? 0U :
            (uint32_t)(((uint64_t)lane->diag_pdm_ones * 1000ULL) / total_bits);
        uint32_t edge_permille = total_bits <= 1U ? 0U :
            (uint32_t)(((uint64_t)lane->diag_pdm_edges * 1000ULL) / (uint64_t)(total_bits - 1U));
        uint32_t pcm_avg_abs = lane->diag_pcm_samples == 0U ? 0U :
            (uint32_t)(lane->diag_pcm_abs_sum / lane->diag_pcm_samples);

        mic_log_mono_flow_diag(lane, ones_permille, edge_permille, pcm_avg_abs);
    }

    for (uint32_t i = 0; i < BSP_MIC_CAPTURE_LANE_COUNT; ++i) {
        s_mic.lane[i].diag_raw_blocks = 0U;
        s_mic.lane[i].diag_raw_bits = 0U;
        s_mic.lane[i].diag_raw_bytes = 0U;
        s_mic.lane[i].diag_pdm_ones = 0U;
        s_mic.lane[i].diag_pdm_edges = 0U;
        s_mic.lane[i].diag_pcm_samples = 0U;
        s_mic.lane[i].diag_pcm_peak = 0U;
        s_mic.lane[i].diag_pcm_abs_sum = 0U;
        s_mic.lane[i].diag_prev_bit_valid = false;
    }
    s_mic.diag_capture_loops = 0U;
    s_mic.diag_i2s_reads = 0U;
    s_mic.diag_i2s_zero_reads = 0U;
    s_mic.diag_i2s_short_reads = 0U;
    s_mic.diag_i2s_odd_reads = 0U;
    s_mic.diag_raw_truncated_blocks = 0U;
    s_mic.diag_pcm_zero_blocks = 0U;
    s_mic.diag_i2s_bytes = 0U;
    s_mic.last_diag_log_us = now_us;
}

static esp_err_t mic_capture_block(spi_device_handle_t spi, uint8_t *rx_buffer)
{
    spi_transaction_t trans = {
        .length = BSP_MIC_RAW_BYTES * 8U,
        .rxlength = BSP_MIC_RAW_BYTES * 8U,
        .tx_buffer = s_mic.tx_dummy,
        .rx_buffer = rx_buffer,
    };

    memset(rx_buffer, 0, BSP_MIC_RAW_BYTES);
    return spi_device_polling_transmit(spi, &trans);
}

static esp_err_t mic_capture_block_i2s(uint8_t *rx_buffer, size_t *bytes_read)
{
    *bytes_read = 0U;
    return i2s_channel_read(s_mic.i2s_rx, rx_buffer, BSP_MIC_RAW_BYTES, bytes_read, 1000);
}

static void mic_report_overflow(size_t pcm_bytes, size_t sent)
{
    if (sent == pcm_bytes) {
        return;
    }

    const uint32_t dropped = (uint32_t)(pcm_bytes - sent);
    const int64_t now_us = esp_timer_get_time();
    s_mic.overflow_events++;
    s_mic.overflow_bytes += dropped;
    if ((now_us - s_mic.last_overflow_log_us) >= BSP_MIC_OVERFLOW_LOG_PERIOD_US) {
        ESP_LOGW(TAG, "Microphone PCM buffer overflow: events=%u dropped=%uB in last %u ms",
                 (unsigned int)s_mic.overflow_events,
                 (unsigned int)s_mic.overflow_bytes,
                 (unsigned int)(BSP_MIC_OVERFLOW_LOG_PERIOD_US / 1000LL));
        s_mic.last_overflow_log_us = now_us;
        s_mic.overflow_events = 0U;
        s_mic.overflow_bytes = 0U;
    }
}

static void mic_capture_task(void *arg)
{
    (void)arg;

    uint32_t loop_count = 0U;

    while (s_mic.task_running) {
        s_mic.diag_capture_loops++;
        if (s_mic.channels > 1U) {
            size_t lane_pcm_count[BSP_MIC_CAPTURE_LANE_COUNT] = {0U};
            esp_err_t ret = mic_capture_block(s_mic.spi[BSP_MIC_STEREO_SHARED_SPI_INDEX],
                                              s_mic.rx_dma[BSP_MIC_STEREO_SHARED_SPI_INDEX]);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Stereo shared-data SPI microphone capture stopped: %s",
                         esp_err_to_name(ret));
                s_microphone_ready = false;
                s_mic.task_running = false;
                s_mic.task = NULL;
                vTaskDelete(NULL);
                return;
            }

            mic_accumulate_interleaved_raw_diag(
                (const uint8_t *)s_mic.rx_dma[BSP_MIC_STEREO_SHARED_SPI_INDEX],
                BSP_MIC_RAW_BYTES);
            mic_pdm_interleaved_to_pcm_stereo(
                (const uint8_t *)s_mic.rx_dma[BSP_MIC_STEREO_SHARED_SPI_INDEX],
                BSP_MIC_RAW_BYTES,
                s_mic.decimation,
                s_mic.lane_pcm[BSP_MIC_LEFT_LANE_INDEX],
                sizeof(s_mic.lane_pcm[BSP_MIC_LEFT_LANE_INDEX]) /
                    sizeof(s_mic.lane_pcm[BSP_MIC_LEFT_LANE_INDEX][0]),
                &lane_pcm_count[BSP_MIC_LEFT_LANE_INDEX],
                s_mic.lane_pcm[BSP_MIC_RIGHT_LANE_INDEX],
                sizeof(s_mic.lane_pcm[BSP_MIC_RIGHT_LANE_INDEX]) /
                    sizeof(s_mic.lane_pcm[BSP_MIC_RIGHT_LANE_INDEX][0]),
                &lane_pcm_count[BSP_MIC_RIGHT_LANE_INDEX]);
            mic_accumulate_pcm_diag(&s_mic.lane[BSP_MIC_LEFT_LANE_INDEX],
                                    s_mic.lane_pcm[BSP_MIC_LEFT_LANE_INDEX],
                                    lane_pcm_count[BSP_MIC_LEFT_LANE_INDEX]);
            mic_accumulate_pcm_diag(&s_mic.lane[BSP_MIC_RIGHT_LANE_INDEX],
                                    s_mic.lane_pcm[BSP_MIC_RIGHT_LANE_INDEX],
                                    lane_pcm_count[BSP_MIC_RIGHT_LANE_INDEX]);

            size_t frame_count = lane_pcm_count[BSP_MIC_LEFT_LANE_INDEX];
            if (lane_pcm_count[BSP_MIC_RIGHT_LANE_INDEX] < frame_count) {
                frame_count = lane_pcm_count[BSP_MIC_RIGHT_LANE_INDEX];
            }

            if (frame_count > 0U) {
                for (size_t frame = 0; frame < frame_count; ++frame) {
                    s_mic.stereo_pcm[(frame * 2U)] = s_mic.lane_pcm[BSP_MIC_LEFT_LANE_INDEX][frame];
                    s_mic.stereo_pcm[(frame * 2U) + 1U] =
                        s_mic.lane_pcm[BSP_MIC_RIGHT_LANE_INDEX][frame];
                }

                size_t pcm_bytes = frame_count * 2U * sizeof(int16_t);
                size_t sent = xStreamBufferSend(s_mic.pcm_stream, s_mic.stereo_pcm, pcm_bytes, 0);
                mic_report_overflow(pcm_bytes, sent);
            }
        } else {
            const uint32_t lane_index = BSP_MIC_MONO_LANE_INDEX;
            size_t captured_bytes = 0U;
            esp_err_t ret = mic_capture_block_i2s(s_mic.rx_dma[lane_index], &captured_bytes);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "I2S raw microphone capture stopped: %s", esp_err_to_name(ret));
                s_microphone_ready = false;
                s_mic.task_running = false;
                s_mic.task = NULL;
                vTaskDelete(NULL);
                return;
            }

            s_mic.diag_i2s_reads++;
            s_mic.diag_i2s_bytes += (uint32_t)captured_bytes;
            s_mic.diag_last_i2s_bytes = (uint32_t)captured_bytes;
            if (captured_bytes == 0U) {
                s_mic.diag_i2s_zero_reads++;
            }
            if (captured_bytes < BSP_MIC_RAW_BYTES) {
                s_mic.diag_i2s_short_reads++;
            }
            if ((captured_bytes & 1U) != 0U) {
                s_mic.diag_i2s_odd_reads++;
            }

            size_t raw_len = mic_normalize_i2s_words_to_raw_bytes(
                (const uint8_t *)s_mic.rx_dma[lane_index], captured_bytes,
                s_mic.raw_pdm_bytes, sizeof(s_mic.raw_pdm_bytes));
            s_mic.diag_last_raw_bytes = (uint32_t)raw_len;
            if (raw_len < captured_bytes) {
                s_mic.diag_raw_truncated_blocks++;
            }
            mic_accumulate_raw_diag(&s_mic.lane[lane_index],
                                    s_mic.raw_pdm_bytes,
                                    raw_len);
            size_t pcm_count = mic_pdm_to_pcm_lane(
                &s_mic.lane[lane_index],
                s_mic.raw_pdm_bytes, raw_len,
                s_mic.decimation, s_mic.lane_pcm[lane_index],
                sizeof(s_mic.lane_pcm[lane_index]) / sizeof(s_mic.lane_pcm[lane_index][0]));
            s_mic.diag_last_pcm_samples = (uint32_t)pcm_count;
            if (pcm_count == 0U) {
                s_mic.diag_pcm_zero_blocks++;
            }
            mic_accumulate_pcm_diag(&s_mic.lane[lane_index], s_mic.lane_pcm[lane_index], pcm_count);

            if (pcm_count > 0U) {
                size_t pcm_bytes = pcm_count * sizeof(int16_t);
                size_t sent =
                    xStreamBufferSend(s_mic.pcm_stream, s_mic.lane_pcm[lane_index], pcm_bytes, 0);
                mic_report_overflow(pcm_bytes, sent);
            }

            const int64_t now_us = esp_timer_get_time();
            if ((captured_bytes == 0U || raw_len == 0U || pcm_count == 0U) &&
                (now_us - s_mic.last_capture_warn_log_us) >= BSP_MIC_DIAG_LOG_PERIOD_US) {
                ESP_LOGW(TAG,
                         "Mic capture anomaly: captured=%u raw=%u pcm=%u warmup_left=%u",
                         (unsigned int)captured_bytes,
                         (unsigned int)raw_len,
                         (unsigned int)pcm_count,
                         (unsigned int)s_mic.lane[lane_index].warmup_samples_remaining);
                s_mic.last_capture_warn_log_us = now_us;
            }
        }

        mic_log_diag_if_due();

        loop_count++;
        if ((loop_count % BSP_MIC_YIELD_EVERY_LOOPS) == 0U) {
            vTaskDelay(1);
        } else {
            taskYIELD();
        }
    }

    s_mic.task = NULL;
    vTaskDelete(NULL);
}

static void mic_cleanup_on_error(void)
{
    if (s_mic.i2s_rx != NULL) {
        i2s_del_channel(s_mic.i2s_rx);
        s_mic.i2s_rx = NULL;
    }

    for (uint32_t i = 0; i < BSP_MIC_CAPTURE_LANE_COUNT; ++i) {
        if (s_mic.spi[i] != NULL) {
            spi_bus_remove_device(s_mic.spi[i]);
            s_mic.spi[i] = NULL;
        }
    }

    spi_bus_free(BSP_MIC_SPI_HOST);

    if (s_mic.pcm_stream != NULL) {
        vStreamBufferDelete(s_mic.pcm_stream);
        s_mic.pcm_stream = NULL;
    }

    for (uint32_t i = 0; i < BSP_MIC_CAPTURE_LANE_COUNT; ++i) {
        if (s_mic.rx_dma[i] != NULL) {
            heap_caps_free(s_mic.rx_dma[i]);
            s_mic.rx_dma[i] = NULL;
        }
    }

    if (s_mic.tx_dummy != NULL) {
        heap_caps_free(s_mic.tx_dummy);
        s_mic.tx_dummy = NULL;
    }
}
#endif

void bsp_microphone_deinit(void)
{
#if !CONFIG_COLLAR_MICROPHONE_ENABLE
    return;
#else
    s_microphone_ready = false;
    s_mic.task_running = false;

    if (s_mic.i2s_rx != NULL) {
        (void)i2s_channel_disable(s_mic.i2s_rx);
    }

    const int64_t deadline_us = esp_timer_get_time() + 200000LL;
    while (s_mic.task != NULL && esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_mic.task != NULL) {
        vTaskDelete(s_mic.task);
        s_mic.task = NULL;
    }

    mic_cleanup_on_error();
    memset(&s_mic, 0, sizeof(s_mic));
#endif
}

esp_err_t bsp_microphone_init(void)
{
#if !CONFIG_COLLAR_MICROPHONE_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_microphone_ready) {
        return ESP_OK;
    }

    memset(&s_mic, 0, sizeof(s_mic));
    s_mic.sample_rate_hz = (uint32_t)CONFIG_COLLAR_MICROPHONE_SAMPLE_RATE;
#if CONFIG_COLLAR_MICROPHONE_STEREO
    s_mic.channels = 2U;
#else
    s_mic.channels = 1U;
#endif
    s_mic.decimation = mic_compute_decimation(s_mic.sample_rate_hz);
    s_mic.capture_clock_hz = s_mic.sample_rate_hz * s_mic.decimation * s_mic.channels;

    if (s_mic.capture_clock_hz > BSP_MIC_MAX_PDM_CLOCK_HZ) {
        ESP_LOGE(TAG, "Requested microphone mode needs unsupported PDM clock %u Hz",
                 (unsigned int)s_mic.capture_clock_hz);
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (uint32_t i = 0; i < BSP_MIC_CAPTURE_LANE_COUNT; ++i) {
        mic_reset_lane_state(&s_mic.lane[i], s_mic.sample_rate_hz);
    }

#if !CONFIG_COLLAR_MICROPHONE_STEREO && CONFIG_COLLAR_SPEAKER_ENABLE
    ESP_LOGI(TAG,
             "Microphone RX and speaker TX share the ESP32-C3 I2S peripheral; duplex is allowed, but monitor audio load under stress");
#endif

#if !CONFIG_COLLAR_MICROPHONE_STEREO
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = 6;
    rx_chan_cfg.dma_frame_num = 511;
    esp_err_t ret = i2s_new_channel(&rx_chan_cfg, NULL, &s_mic.i2s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S raw RX channel: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG,
             "Mic I2S RX config: port=%d dma_desc=%u dma_frame=%u clk_gpio=%d din_gpio=%d",
             I2S_NUM_0,
             (unsigned int)rx_chan_cfg.dma_desc_num,
             (unsigned int)rx_chan_cfg.dma_frame_num,
             CONFIG_COLLAR_MICROPHONE_CLK_GPIO,
             CONFIG_COLLAR_MICROPHONE_DIN_GPIO);
#endif

    const uint32_t capture_lane_count = s_mic.channels > 1U ? BSP_MIC_CAPTURE_LANE_COUNT : 1U;
#if CONFIG_COLLAR_MICROPHONE_STEREO
    s_mic.tx_dummy = heap_caps_calloc(1, BSP_MIC_RAW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_mic.tx_dummy == NULL) {
        ESP_LOGE(TAG, "Failed to allocate DMA TX buffer (%uB)", (unsigned int)BSP_MIC_RAW_BYTES);
        mic_cleanup_on_error();
        return ESP_ERR_NO_MEM;
    }
#endif

    for (uint32_t i = 0; i < capture_lane_count; ++i) {
        s_mic.rx_dma[i] = heap_caps_calloc(1, BSP_MIC_RAW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_mic.rx_dma[i] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate DMA RX buffer for lane %u (%uB)",
                     (unsigned int)i, (unsigned int)BSP_MIC_RAW_BYTES);
            mic_cleanup_on_error();
            return ESP_ERR_NO_MEM;
        }
    }

    const size_t pcm_stream_bytes = mic_stream_buffer_bytes(s_mic.sample_rate_hz, s_mic.channels);
    s_mic.pcm_stream = xStreamBufferCreate(pcm_stream_bytes, sizeof(int16_t));
    if (s_mic.pcm_stream == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PCM stream buffer (%uB for %u ch @ %u Hz)",
                 (unsigned int)pcm_stream_bytes,
                 (unsigned int)s_mic.channels,
                 (unsigned int)s_mic.sample_rate_hz);
        mic_cleanup_on_error();
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_COLLAR_MICROPHONE_STEREO
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = -1,
        .miso_io_num = CONFIG_COLLAR_MICROPHONE_DIN_GPIO,
        .sclk_io_num = CONFIG_COLLAR_MICROPHONE_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = BSP_MIC_RAW_BYTES,
    };

    esp_err_t ret = spi_bus_initialize(BSP_MIC_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus for microphone: %s", esp_err_to_name(ret));
        mic_cleanup_on_error();
        return ret;
    }

    const int spi_mode[BSP_MIC_CAPTURE_LANE_COUNT] = {0, 0};
#else
#ifdef CONFIG_COLLAR_MICROPHONE_LR_HIGH
    const int spi_mode[BSP_MIC_CAPTURE_LANE_COUNT] = {1, 1};
#else
    const int spi_mode[BSP_MIC_CAPTURE_LANE_COUNT] = {0, 0};
#endif
#endif

    const uint32_t spi_device_count = s_mic.channels > 1U ? 1U : 0U;
    for (uint32_t lane_index = 0; lane_index < spi_device_count; ++lane_index) {
        spi_device_interface_config_t dev_cfg = {
            .command_bits = 0,
            .address_bits = 0,
            .dummy_bits = 0,
            .mode = spi_mode[lane_index],
            .clock_source = SPI_CLK_SRC_DEFAULT,
            .duty_cycle_pos = 128,
            .cs_ena_pretrans = 0,
            .cs_ena_posttrans = 0,
            .clock_speed_hz = (int)s_mic.capture_clock_hz,
            .input_delay_ns = 0,
            .spics_io_num = -1,
            .flags = 0,
            .queue_size = 1,
            .pre_cb = NULL,
            .post_cb = NULL,
        };

        ret = spi_bus_add_device(BSP_MIC_SPI_HOST, &dev_cfg, &s_mic.spi[lane_index]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add SPI microphone device on lane %u: %s",
                     (unsigned int)lane_index, esp_err_to_name(ret));
            mic_cleanup_on_error();
            return ret;
        }
    }

#if !CONFIG_COLLAR_MICROPHONE_STEREO
    const uint32_t i2s_raw_rate_hz = mic_i2s_raw_sample_rate_hz(s_mic.capture_clock_hz);
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(i2s_raw_rate_hz),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_COLLAR_MICROPHONE_CLK_GPIO,
            .ws = I2S_GPIO_UNUSED,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_COLLAR_MICROPHONE_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(s_mic.i2s_rx, &rx_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S raw RX mode: %s", esp_err_to_name(ret));
        mic_cleanup_on_error();
        return ret;
    }
    ret = i2s_channel_enable(s_mic.i2s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S raw RX mode: %s", esp_err_to_name(ret));
        mic_cleanup_on_error();
        return ret;
    }
    ESP_LOGI(TAG,
             "Mic I2S STD mode: sample_rate=%uHz slot_bits=%u slots=%u ws=%d bclk=%d din=%d",
             (unsigned int)i2s_raw_rate_hz,
             (unsigned int)BSP_MIC_I2S_SLOT_BITS,
             (unsigned int)BSP_MIC_I2S_SLOT_COUNT,
             I2S_GPIO_UNUSED,
             CONFIG_COLLAR_MICROPHONE_CLK_GPIO,
             CONFIG_COLLAR_MICROPHONE_DIN_GPIO);
#endif

    s_mic.task_running = true;
    ret = xTaskCreate(
        mic_capture_task, "mic_capture", BSP_MIC_TASK_STACK_WORDS, NULL,
        BSP_MIC_TASK_PRIORITY, &s_mic.task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mic_capture task (stack=%u)",
                 (unsigned int)BSP_MIC_TASK_STACK_WORDS);
        s_mic.task_running = false;
        mic_cleanup_on_error();
        return ESP_ERR_NO_MEM;
    }

    s_microphone_ready = true;
    if (s_mic.channels > 1U) {
        ESP_LOGI(TAG,
                 "Microphone input ready: clk=%d din=%d rate=%uHz channels=2 mode=spi-pdm stereo16-shared raw_clk=%uHz decimation=%u demux=(even:left,odd:right)",
                 CONFIG_COLLAR_MICROPHONE_CLK_GPIO,
                 CONFIG_COLLAR_MICROPHONE_DIN_GPIO,
                 (unsigned int)s_mic.sample_rate_hz,
                 (unsigned int)s_mic.capture_clock_hz,
                 (unsigned int)s_mic.decimation);
    } else {
#ifdef CONFIG_COLLAR_MICROPHONE_LR_HIGH
        const char *mono_slot = "right";
#else
        const char *mono_slot = "left";
#endif
        ESP_LOGI(TAG,
                 "Microphone input ready: clk=%d din=%d rate=%uHz channels=1 mode=i2s-raw mono16 raw_clk=%uHz decimation=%u i2s_rate=%uHz slot=%s",
                 CONFIG_COLLAR_MICROPHONE_CLK_GPIO,
                 CONFIG_COLLAR_MICROPHONE_DIN_GPIO,
                 (unsigned int)s_mic.sample_rate_hz,
                 (unsigned int)s_mic.capture_clock_hz,
                 (unsigned int)s_mic.decimation,
                 (unsigned int)mic_i2s_raw_sample_rate_hz(s_mic.capture_clock_hz),
                 mono_slot);
    }
    return ESP_OK;
#endif
}

bool bsp_microphone_is_ready(void)
{
#if !CONFIG_COLLAR_MICROPHONE_ENABLE
    return false;
#else
    return s_microphone_ready && s_mic.pcm_stream != NULL;
#endif
}

uint32_t bsp_microphone_get_sample_rate_hz(void)
{
#if !CONFIG_COLLAR_MICROPHONE_ENABLE
    return 0U;
#else
    return (uint32_t)CONFIG_COLLAR_MICROPHONE_SAMPLE_RATE;
#endif
}

uint8_t bsp_microphone_get_channels(void)
{
#if !CONFIG_COLLAR_MICROPHONE_ENABLE
    return 0U;
#else
#if CONFIG_COLLAR_MICROPHONE_STEREO
    return 2U;
#else
    return 1U;
#endif
#endif
}

size_t bsp_microphone_frame_bytes(void)
{
    return (size_t)bsp_microphone_get_channels() * sizeof(int16_t);
}

esp_err_t bsp_microphone_read(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
#if !CONFIG_COLLAR_MICROPHONE_ENABLE
    (void)data;
    (void)len;
    (void)bytes_read;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!bsp_microphone_is_ready() || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t total = 0U;
    TickType_t remaining_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t start_tick = xTaskGetTickCount();

    while (total < len) {
        size_t got = xStreamBufferReceive(
            s_mic.pcm_stream, ((uint8_t *)data) + total, len - total, remaining_ticks);
        if (got == 0U) {
            break;
        }
        total += got;

        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed >= pdMS_TO_TICKS(timeout_ms)) {
            remaining_ticks = 0;
        } else {
            remaining_ticks = pdMS_TO_TICKS(timeout_ms) - elapsed;
        }
    }

    if (bytes_read != NULL) {
        *bytes_read = total;
    }

    return total > 0U ? ESP_OK : ESP_ERR_TIMEOUT;
#endif
}
