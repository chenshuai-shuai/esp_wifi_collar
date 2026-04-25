#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "app/app_manager.h"
#include "bsp/bsp_board.h"
#include "kernel/kernel.h"
#include "kernel/kernel_msgbus.h"
#include "platform_hal/log_control.h"
#include "services/service_manager.h"

#include "firmware_version.h"


static const char *TAG = "main";

static const char *reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "other_watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power_glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu_lockup";
    default:
        return "unmapped";
    }
}

static void log_boot_identity(void)
{
    esp_chip_info_t chip = {0};
    const esp_app_desc_t *app_desc = esp_app_get_description();
    esp_reset_reason_t reset_reason = esp_reset_reason();

    esp_chip_info(&chip);

    /*
     * Loud, easy-to-grep firmware tag. Bump FW_VERSION_BUILD in
     * firmware_version.h whenever we ship a behavioural change, so
     * looking at the boot banner immediately reveals stale flash.
     * Compile-time __DATE__/__TIME__ anchors the log to a specific
     * build artefact even when the in-app tag is forgotten to be
     * bumped.
     */
    ESP_LOGI(TAG, "FW-VER: %s (build=%d) compiled %s %s",
             FW_VERSION_STRING, FW_VERSION_BUILD, __DATE__, __TIME__);
    ESP_LOGI(TAG, "Boot start: project=%s version=%s idf=%s",
             app_desc->project_name, app_desc->version, esp_get_idf_version());

    ESP_LOGI(TAG, "Chip info: model=%s cores=%d revision=%d features=0x%" PRIx32,
             CONFIG_IDF_TARGET, chip.cores, chip.revision, chip.features);
    ESP_LOGI(TAG, "Reset reason: %s (%d)",
             reset_reason_to_string(reset_reason), reset_reason);
}

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
    log_control_apply();
    log_boot_identity();
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
