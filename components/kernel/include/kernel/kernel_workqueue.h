#pragma once

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

typedef void (*kernel_work_fn_t)(void *ctx);

typedef struct {
    kernel_work_fn_t fn;
    void *ctx;
} kernel_work_item_t;

esp_err_t kernel_workqueue_init(void);
esp_err_t kernel_workqueue_post(const kernel_work_item_t *item, TickType_t wait_ticks);
