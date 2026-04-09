#include "bsp/bsp_board.h"

#include "esp_log.h"

#include "platform_hal/hal_system.h"

static const char *TAG = "bsp";

esp_err_t bsp_board_init(void)
{
    ESP_LOGI(TAG, "Board init: esp_wifi_collar");
    return hal_platform_init();
}
