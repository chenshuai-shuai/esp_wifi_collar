#include "audio_dsp/audio_denoise.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "speex/speex_preprocess.h"

#define AUDIO_DENOISE_MAX_CHANNELS 2U
#define AUDIO_DENOISE_STATUS_LOG_US 5000000LL

static const char *TAG = "audio_denoise";

typedef struct {
    bool initialized;
    audio_denoise_config_t config;
    size_t frame_samples;
    SpeexPreprocessState *preprocess[AUDIO_DENOISE_MAX_CHANNELS];
    int16_t *channel_frame[AUDIO_DENOISE_MAX_CHANNELS];
    uint32_t processed_blocks;
    uint32_t speech_blocks;
    int64_t last_status_log_us;
} audio_denoise_state_t;

static audio_denoise_state_t s_denoise;

static bool audio_denoise_config_equal(const audio_denoise_config_t *lhs,
                                       const audio_denoise_config_t *rhs)
{
    return lhs->sample_rate_hz == rhs->sample_rate_hz &&
           lhs->channels == rhs->channels &&
           lhs->frame_ms == rhs->frame_ms &&
           lhs->noise_suppress_db == rhs->noise_suppress_db &&
           lhs->vad_enabled == rhs->vad_enabled;
}

static void audio_denoise_cleanup(void)
{
    for (size_t ch = 0; ch < AUDIO_DENOISE_MAX_CHANNELS; ++ch) {
        if (s_denoise.preprocess[ch] != NULL) {
            speex_preprocess_state_destroy(s_denoise.preprocess[ch]);
            s_denoise.preprocess[ch] = NULL;
        }
        free(s_denoise.channel_frame[ch]);
        s_denoise.channel_frame[ch] = NULL;
    }
    memset(&s_denoise, 0, sizeof(s_denoise));
}

esp_err_t audio_denoise_init(const audio_denoise_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->channels == 0U || config->channels > AUDIO_DENOISE_MAX_CHANNELS ||
        config->frame_ms == 0U || config->sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_denoise.initialized && audio_denoise_config_equal(&s_denoise.config, config)) {
        return ESP_OK;
    }

    audio_denoise_cleanup();

    const uint64_t frame_samples_exact =
        ((uint64_t)config->sample_rate_hz * (uint64_t)config->frame_ms);
    if ((frame_samples_exact % 1000ULL) != 0ULL) {
        ESP_LOGE(TAG, "Unsupported frame config: rate=%lu frame_ms=%u does not land on whole samples",
                 (unsigned long)config->sample_rate_hz,
                 (unsigned int)config->frame_ms);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_denoise.frame_samples = (size_t)(frame_samples_exact / 1000ULL);
    if (s_denoise.frame_samples == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_denoise.config = *config;

    for (size_t ch = 0; ch < config->channels; ++ch) {
        s_denoise.preprocess[ch] =
            speex_preprocess_state_init((int)s_denoise.frame_samples, (int)config->sample_rate_hz);
        if (s_denoise.preprocess[ch] == NULL) {
            audio_denoise_cleanup();
            return ESP_ERR_NO_MEM;
        }

        s_denoise.channel_frame[ch] =
            (int16_t *)calloc(s_denoise.frame_samples, sizeof(int16_t));
        if (s_denoise.channel_frame[ch] == NULL) {
            audio_denoise_cleanup();
            return ESP_ERR_NO_MEM;
        }

        int32_t enabled = 1;
        (void)speex_preprocess_ctl(s_denoise.preprocess[ch], SPEEX_PREPROCESS_SET_DENOISE, &enabled);
        int32_t noise_suppress_db = config->noise_suppress_db;
        (void)speex_preprocess_ctl(s_denoise.preprocess[ch],
                                   SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,
                                   &noise_suppress_db);
        int32_t vad_enabled = config->vad_enabled ? 1 : 0;
        (void)speex_preprocess_ctl(s_denoise.preprocess[ch], SPEEX_PREPROCESS_SET_VAD, &vad_enabled);
    }

    s_denoise.initialized = true;
    ESP_LOGI(TAG,
             "Mic denoise ready: backend=speexdsp mode=fixed-point rate=%lu channels=%u frame=%ums samples=%u noise_suppress=%lddB vad=%s",
             (unsigned long)config->sample_rate_hz,
             (unsigned int)config->channels,
             (unsigned int)config->frame_ms,
             (unsigned int)s_denoise.frame_samples,
             (long)config->noise_suppress_db,
             config->vad_enabled ? "on" : "off");
    return ESP_OK;
}

void audio_denoise_deinit(void)
{
    audio_denoise_cleanup();
}

bool audio_denoise_is_ready(void)
{
    return s_denoise.initialized;
}

size_t audio_denoise_frame_samples(void)
{
    return s_denoise.frame_samples;
}

esp_err_t audio_denoise_process_interleaved(int16_t *samples, size_t frame_count, size_t *speech_blocks)
{
    if (!s_denoise.initialized || samples == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t block_frames = s_denoise.frame_samples;
    if (block_frames == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t complete_blocks = frame_count / block_frames;
    if (speech_blocks != NULL) {
        *speech_blocks = 0U;
    }

    for (size_t block = 0; block < complete_blocks; ++block) {
        const size_t frame_offset = block * block_frames;
        size_t block_speech = 0U;

        if (s_denoise.config.channels == 1U) {
            const int vad = speex_preprocess_run(s_denoise.preprocess[0], &samples[frame_offset]);
            if (s_denoise.config.vad_enabled && vad != 0) {
                block_speech = 1U;
            }
        } else {
            for (size_t ch = 0; ch < s_denoise.config.channels; ++ch) {
                int16_t *channel_frame = s_denoise.channel_frame[ch];
                for (size_t i = 0; i < block_frames; ++i) {
                    channel_frame[i] = samples[((frame_offset + i) * s_denoise.config.channels) + ch];
                }

                const int vad = speex_preprocess_run(s_denoise.preprocess[ch], channel_frame);
                if (s_denoise.config.vad_enabled && vad != 0) {
                    block_speech = 1U;
                }

                for (size_t i = 0; i < block_frames; ++i) {
                    samples[((frame_offset + i) * s_denoise.config.channels) + ch] = channel_frame[i];
                }
            }
        }

        s_denoise.processed_blocks++;
        s_denoise.speech_blocks += (uint32_t)block_speech;
        if (speech_blocks != NULL) {
            *speech_blocks += block_speech;
        }
    }

    return ESP_OK;
}

void audio_denoise_log_status_if_due(void)
{
    if (!s_denoise.initialized) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    if ((now_us - s_denoise.last_status_log_us) < AUDIO_DENOISE_STATUS_LOG_US) {
        return;
    }

    s_denoise.last_status_log_us = now_us;
    ESP_LOGI(TAG,
             "Mic denoise status: backend=speexdsp blocks=%lu speech=%lu frame=%u noise_suppress=%lddB vad=%s",
             (unsigned long)s_denoise.processed_blocks,
             (unsigned long)s_denoise.speech_blocks,
             (unsigned int)s_denoise.frame_samples,
             (long)s_denoise.config.noise_suppress_db,
             s_denoise.config.vad_enabled ? "on" : "off");
}
