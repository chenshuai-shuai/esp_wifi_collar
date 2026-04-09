#include "app/app_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "kernel/kernel_msgbus.h"
#include "kernel/kernel_trace.h"

#define APP_MANAGER_STACK_WORDS        2048
#define APP_MANAGER_PRIORITY           8
#define APP_MANAGER_CORE               1
#define APP_HEARTBEAT_PERIOD_MS        1000

static const char *TAG = "app_mgr";

static StaticTask_t s_app_tcb;
static StackType_t s_app_stack[APP_MANAGER_STACK_WORDS];
static bool s_app_started;

static void collar_app_task(void *arg)
{
    (void)arg;

    uint32_t heartbeat_seq = 0;

    for (;;) {
        heartbeat_seq++;

        const kernel_msg_t msg = {
            .topic = KERNEL_TOPIC_APP_HEARTBEAT,
            .source = KERNEL_SOURCE_APP,
            .value = heartbeat_seq,
            .timestamp_us = esp_timer_get_time(),
        };
        (void)kernel_msgbus_publish(&msg, pdMS_TO_TICKS(10));

        if ((heartbeat_seq % 10U) == 0U) {
            kernel_trace_counter("app_heartbeat", heartbeat_seq);
        }

        ESP_LOGI(TAG, "Collar app alive seq=%lu", (unsigned long)heartbeat_seq);
        vTaskDelay(pdMS_TO_TICKS(APP_HEARTBEAT_PERIOD_MS));
    }
}

esp_err_t app_manager_start(void)
{
    if (s_app_started) {
        return ESP_OK;
    }

    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        collar_app_task,
        "collar_app",
        APP_MANAGER_STACK_WORDS,
        NULL,
        APP_MANAGER_PRIORITY,
        s_app_stack,
        &s_app_tcb,
        APP_MANAGER_CORE
    );

    if (task_handle == NULL) {
        return ESP_FAIL;
    }

    s_app_started = true;
    kernel_trace_boot("app manager started");
    return ESP_OK;
}
