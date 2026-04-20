#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t bsp_speaker_init(void);
bool bsp_speaker_is_ready(void);
uint32_t bsp_speaker_get_sample_rate_hz(void);
esp_err_t bsp_speaker_write(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms);
