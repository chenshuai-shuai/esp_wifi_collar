#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t bsp_microphone_init(void);
void bsp_microphone_deinit(void);
bool bsp_microphone_is_ready(void);
uint32_t bsp_microphone_get_sample_rate_hz(void);
uint8_t bsp_microphone_get_channels(void);
size_t bsp_microphone_frame_bytes(void);
esp_err_t bsp_microphone_read(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);
