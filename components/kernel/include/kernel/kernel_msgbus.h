#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

typedef enum {
    KERNEL_TOPIC_SYSTEM_BOOT = 1,
    KERNEL_TOPIC_RT_CYCLE,
    KERNEL_TOPIC_APP_HEARTBEAT,
    KERNEL_TOPIC_SERVICE_HEALTH,
    KERNEL_TOPIC_WIFI_STATE,
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

typedef enum {
    KERNEL_WIFI_STATE_IDLE = 0,
    KERNEL_WIFI_STATE_STARTING,
    KERNEL_WIFI_STATE_PROVISIONING,
    KERNEL_WIFI_STATE_CONNECTING,
    KERNEL_WIFI_STATE_CONNECTED,
    KERNEL_WIFI_STATE_GOT_IP,
    KERNEL_WIFI_STATE_DISCONNECTED,
    KERNEL_WIFI_STATE_FAILED,
} kernel_wifi_state_t;

esp_err_t kernel_msgbus_init(void);
esp_err_t kernel_msgbus_publish(const kernel_msg_t *msg, TickType_t wait_ticks);
BaseType_t kernel_msgbus_receive(kernel_msg_t *msg, TickType_t wait_ticks);
