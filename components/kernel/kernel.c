#include "kernel/kernel.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "kernel/kernel_msgbus.h"
#include "kernel/kernel_supervisor.h"
#include "kernel/kernel_trace.h"
#include "kernel/kernel_workqueue.h"

#define KERNEL_RT_TASK_STACK_WORDS     2048
#define KERNEL_RT_TASK_PRIORITY        22
#define KERNEL_RT_TASK_CORE            0
#define KERNEL_RT_PERIOD_US            10000
#define KERNEL_RT_PUBLISH_INTERVAL     100

static const char *TAG = "kernel";

static TaskHandle_t s_rt_task_handle;
static StaticTask_t s_rt_task_tcb;
static StackType_t s_rt_task_stack[KERNEL_RT_TASK_STACK_WORDS];
static esp_timer_handle_t s_rt_timer_handle;
static bool s_kernel_initialized;

static void kernel_rt_timer_callback(void *arg)
{
    (void)arg;

    if (s_rt_task_handle != NULL) {
        xTaskNotifyGive(s_rt_task_handle);
    }
}

static void kernel_rt_task(void *arg)
{
    (void)arg;

    uint32_t cycle_count = 0;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        cycle_count++;

        if ((cycle_count % KERNEL_RT_PUBLISH_INTERVAL) == 0U) {
            const kernel_msg_t msg = {
                .topic = KERNEL_TOPIC_RT_CYCLE,
                .source = KERNEL_SOURCE_KERNEL,
                .value = cycle_count,
                .timestamp_us = esp_timer_get_time(),
            };
            (void)kernel_msgbus_publish(&msg, 0);
        }
    }
}

esp_err_t kernel_init(void)
{
    esp_err_t ret;

    if (s_kernel_initialized) {
        return ESP_OK;
    }

    kernel_trace_boot("kernel init begin");

    ret = kernel_msgbus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = kernel_workqueue_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = kernel_supervisor_start();
    if (ret != ESP_OK) {
        return ret;
    }

    s_rt_task_handle = xTaskCreateStaticPinnedToCore(
        kernel_rt_task,
        "kernel_rt",
        KERNEL_RT_TASK_STACK_WORDS,
        NULL,
        KERNEL_RT_TASK_PRIORITY,
        s_rt_task_stack,
        &s_rt_task_tcb,
        KERNEL_RT_TASK_CORE
    );
    if (s_rt_task_handle == NULL) {
        return ESP_FAIL;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = kernel_rt_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "kernel_rt_tick",
        .skip_unhandled_events = true,
    };

    ret = esp_timer_create(&timer_args, &s_rt_timer_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_timer_start_periodic(s_rt_timer_handle, KERNEL_RT_PERIOD_US);
    if (ret != ESP_OK) {
        return ret;
    }

    s_kernel_initialized = true;
    ESP_LOGI(TAG, "Kernel online: rt core=%d period=%dus", KERNEL_RT_TASK_CORE, KERNEL_RT_PERIOD_US);
    kernel_trace_boot("kernel init done");
    return ESP_OK;
}
