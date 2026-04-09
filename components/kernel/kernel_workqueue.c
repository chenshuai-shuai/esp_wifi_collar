#include "kernel/kernel_workqueue.h"

#include <string.h>

#include "freertos/queue.h"
#include "freertos/task.h"

#define KERNEL_WORKQUEUE_LENGTH        16
#define KERNEL_WORKER_STACK_WORDS      2048
#define KERNEL_WORKER_PRIORITY         10
#define KERNEL_WORKER_CORE             1

static StaticQueue_t s_workqueue_tcb;
static uint8_t s_workqueue_storage[KERNEL_WORKQUEUE_LENGTH * sizeof(kernel_work_item_t)];
static QueueHandle_t s_workqueue;

static StaticTask_t s_worker_tcb;
static StackType_t s_worker_stack[KERNEL_WORKER_STACK_WORDS];
static bool s_worker_started;

static void kernel_workqueue_task(void *arg)
{
    (void)arg;

    kernel_work_item_t item;

    for (;;) {
        if (xQueueReceive(s_workqueue, &item, portMAX_DELAY) == pdPASS && item.fn != NULL) {
            item.fn(item.ctx);
        }
    }
}

esp_err_t kernel_workqueue_init(void)
{
    if (s_worker_started) {
        return ESP_OK;
    }

    memset(s_workqueue_storage, 0, sizeof(s_workqueue_storage));
    s_workqueue = xQueueCreateStatic(
        KERNEL_WORKQUEUE_LENGTH,
        sizeof(kernel_work_item_t),
        s_workqueue_storage,
        &s_workqueue_tcb
    );
    if (s_workqueue == NULL) {
        return ESP_FAIL;
    }

    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        kernel_workqueue_task,
        "svc_worker",
        KERNEL_WORKER_STACK_WORDS,
        NULL,
        KERNEL_WORKER_PRIORITY,
        s_worker_stack,
        &s_worker_tcb,
        KERNEL_WORKER_CORE
    );
    if (task_handle == NULL) {
        return ESP_FAIL;
    }

    s_worker_started = true;
    return ESP_OK;
}

esp_err_t kernel_workqueue_post(const kernel_work_item_t *item, TickType_t wait_ticks)
{
    if (item == NULL || item->fn == NULL || s_workqueue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return (xQueueSend(s_workqueue, item, wait_ticks) == pdPASS) ? ESP_OK : ESP_ERR_TIMEOUT;
}
