#include "bsp/bsp_board.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "platform_hal/hal_system.h"

static const char *TAG = "bsp";

esp_err_t bsp_audio_shared_gpio_highz(void)
{
    const gpio_num_t shared_gpios[] = {
        (gpio_num_t)CONFIG_COLLAR_MICROPHONE_CLK_GPIO,
        (gpio_num_t)CONFIG_COLLAR_MICROPHONE_DIN_GPIO,
        (gpio_num_t)CONFIG_COLLAR_SPEAKER_SD_MODE_GPIO,
        (gpio_num_t)CONFIG_COLLAR_SPEAKER_DOUT_GPIO,
        (gpio_num_t)CONFIG_COLLAR_SPEAKER_BCLK_GPIO,
        (gpio_num_t)CONFIG_COLLAR_SPEAKER_LRCLK_GPIO,
    };

    for (size_t i = 0; i < sizeof(shared_gpios) / sizeof(shared_gpios[0]); ++i) {
        esp_err_t ret = gpio_reset_pin(shared_gpios[i]);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = gpio_set_direction(shared_gpios[i], GPIO_MODE_INPUT);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = gpio_set_pull_mode(shared_gpios[i], GPIO_FLOATING);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

esp_err_t bsp_board_init(void)
{
    ESP_LOGI(TAG, "Board init: esp_wifi_collar");
    esp_err_t ret = hal_platform_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = bsp_audio_shared_gpio_highz();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Board init leaves shared audio GPIO high-z until nRF handoff take");

    return ESP_OK;
}
