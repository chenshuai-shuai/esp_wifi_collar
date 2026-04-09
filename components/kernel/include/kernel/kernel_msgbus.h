#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

typedef enum {
    KERNEL_TOPIC_SYSTEM_BOOT = 1,
    KERNEL_TOPIC_RT_CYCLE,
    KERNEL_TOPIC_APP_HEARTBEAT,
    KERNEL_TOPIC_SERVICE_HEALTH,
} kernel_topic_t;

typedef enum {
    KERNEL_SOURCE_KERNEL = 1,
    KERNEL_SOURCE_SERVICE,
    KERNEL_SOURCE_APP,
} kernel_source_t;

typedef struct {
    kernel_topic_t topic;
    kernel_source_t source;
    uint32_t value;
    int64_t timestamp_us;
} kernel_msg_t;

esp_err_t kernel_msgbus_init(void);
esp_err_t kernel_msgbus_publish(const kernel_msg_t *msg, TickType_t wait_ticks);
BaseType_t kernel_msgbus_receive(kernel_msg_t *msg, TickType_t wait_ticks);
