#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "app/app_manager.h"
#include "bsp/bsp_board.h"
#include "kernel/kernel.h"
#include "kernel/kernel_msgbus.h"
#include "services/service_manager.h"

static const char *TAG = "main";

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void app_main(void)
{
    init_nvs();

    ESP_ERROR_CHECK(bsp_board_init());
    ESP_ERROR_CHECK(kernel_init());
    ESP_ERROR_CHECK(service_manager_init());
    ESP_ERROR_CHECK(app_manager_start());

    const kernel_msg_t boot_msg = {
        .topic = KERNEL_TOPIC_SYSTEM_BOOT,
        .source = KERNEL_SOURCE_KERNEL,
        .value = 1,
        .timestamp_us = esp_timer_get_time(),
    };
    ESP_ERROR_CHECK(kernel_msgbus_publish(&boot_msg, pdMS_TO_TICKS(10)));

    ESP_LOGI(TAG, "System scaffold started");
    vTaskDelete(NULL);
}
