#include "kernel/kernel_supervisor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "kernel/kernel_msgbus.h"
#include "kernel/kernel_trace.h"

#define KERNEL_SUPERVISOR_STACK_WORDS  1536
#define KERNEL_SUPERVISOR_PRIORITY     4
#define KERNEL_SUPERVISOR_CORE         1
#define KERNEL_SUPERVISOR_PERIOD_MS    5000

static const char *TAG = "supervisor";

static StaticTask_t s_supervisor_tcb;
static StackType_t s_supervisor_stack[KERNEL_SUPERVISOR_STACK_WORDS];
static bool s_supervisor_started;

static void kernel_supervisor_task(void *arg)
{
    (void)arg;

    for (;;) {
        const uint32_t free_heap = esp_get_free_heap_size();
        const uint32_t min_heap = xPortGetMinimumEverFreeHeapSize();

        ESP_LOGI(TAG, "heap=%lu min_heap=%lu", (unsigned long)free_heap, (unsigned long)min_heap);
        kernel_trace_counter("free_heap", free_heap);

        const kernel_msg_t msg = {
            .topic = KERNEL_TOPIC_SERVICE_HEALTH,
            .source = KERNEL_SOURCE_KERNEL,
            .value = free_heap,
            .timestamp_us = esp_timer_get_time(),
        };
        (void)kernel_msgbus_publish(&msg, 0);

        vTaskDelay(pdMS_TO_TICKS(KERNEL_SUPERVISOR_PERIOD_MS));
    }
}

esp_err_t kernel_supervisor_start(void)
{
    if (s_supervisor_started) {
        return ESP_OK;
    }

    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        kernel_supervisor_task,
        "kernel_sup",
        KERNEL_SUPERVISOR_STACK_WORDS,
        NULL,
        KERNEL_SUPERVISOR_PRIORITY,
        s_supervisor_stack,
        &s_supervisor_tcb,
        KERNEL_SUPERVISOR_CORE
    );

    if (task_handle == NULL) {
        return ESP_FAIL;
    }

    s_supervisor_started = true;
    return ESP_OK;
}
