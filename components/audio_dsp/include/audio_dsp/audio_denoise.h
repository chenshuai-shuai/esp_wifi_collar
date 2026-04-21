#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t frame_ms;
    int32_t noise_suppress_db;
    bool vad_enabled;
} audio_denoise_config_t;

esp_err_t audio_denoise_init(const audio_denoise_config_t *config);
void audio_denoise_deinit(void);
bool audio_denoise_is_ready(void);
size_t audio_denoise_frame_samples(void);
esp_err_t audio_denoise_process_interleaved(int16_t *samples, size_t frame_count, size_t *speech_blocks);
void audio_denoise_log_status_if_due(void);
