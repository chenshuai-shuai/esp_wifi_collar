#pragma once

#include "esp_err.h"

esp_err_t bsp_board_init(void);
esp_err_t bsp_audio_shared_gpio_highz(void);
