#include "bsp/bsp_board.h"
#include "bsp/microphone_input.h"
#include "bsp/speaker_output.h"

#include "esp_log.h"

#include "platform_hal/hal_system.h"

static const char *TAG = "bsp";

esp_err_t bsp_board_init(void)
{
    ESP_LOGI(TAG, "Board init: esp_wifi_collar");
    esp_err_t ret = hal_platform_init();
    if (ret != ESP_OK) {
        return ret;
    }

#if CONFIG_COLLAR_SPEAKER_TEST_ENABLE || CONFIG_COLLAR_MICROPHONE_TEST_ENABLE
    ESP_LOGI(TAG, "Board init defers audio device bring-up to app manager test mode");
#else
#if CONFIG_COLLAR_SPEAKER_ENABLE
    ret = bsp_speaker_init();
    if (ret != ESP_OK) {
        return ret;
    }
#endif

#if CONFIG_COLLAR_MICROPHONE_ENABLE
    ret = bsp_microphone_init();
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Microphone input not supported on this target; board init continuing");
    } else if (ret != ESP_OK) {
        return ret;
    }
#endif
#endif

    return ESP_OK;
}
