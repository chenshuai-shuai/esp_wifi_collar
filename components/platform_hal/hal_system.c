#include "platform_hal/hal_system.h"

#include "esp_chip_info.h"
#include "esp_log.h"

static const char *TAG = "hal";

esp_err_t hal_platform_init(void)
{
    esp_chip_info_t chip_info;

    esp_chip_info(&chip_info);
    ESP_LOGI(
        TAG,
        "Platform init: model=%d cores=%d revision=%d",
        chip_info.model,
        chip_info.cores,
        chip_info.revision
    );

    return ESP_OK;
}
