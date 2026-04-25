#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t dialog_playback_write(const uint8_t *pcm, size_t len);
void dialog_playback_reset_counters(void);
uint32_t dialog_playback_frames_played(void);
