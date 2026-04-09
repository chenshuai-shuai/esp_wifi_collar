#include "kernel/kernel_msgbus.h"

#include <string.h>

#include "freertos/queue.h"

#define KERNEL_MSGBUS_LENGTH 32

static StaticQueue_t s_msgbus_queue_tcb;
static uint8_t s_msgbus_storage[KERNEL_MSGBUS_LENGTH * sizeof(kernel_msg_t)];
static QueueHandle_t s_msgbus_queue;

esp_err_t kernel_msgbus_init(void)
{
    if (s_msgbus_queue != NULL) {
        return ESP_OK;
    }

    memset(s_msgbus_storage, 0, sizeof(s_msgbus_storage));
    s_msgbus_queue = xQueueCreateStatic(
        KERNEL_MSGBUS_LENGTH,
        sizeof(kernel_msg_t),
        s_msgbus_storage,
        &s_msgbus_queue_tcb
    );

    return (s_msgbus_queue != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t kernel_msgbus_publish(const kernel_msg_t *msg, TickType_t wait_ticks)
{
    if (msg == NULL || s_msgbus_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return (xQueueSend(s_msgbus_queue, msg, wait_ticks) == pdPASS) ? ESP_OK : ESP_ERR_TIMEOUT;
}

BaseType_t kernel_msgbus_receive(kernel_msg_t *msg, TickType_t wait_ticks)
{
    if (msg == NULL || s_msgbus_queue == NULL) {
        return pdFALSE;
    }

    return xQueueReceive(s_msgbus_queue, msg, wait_ticks);
}
