#include "kernel/kernel_trace.h"

#include <inttypes.h>

#include "esp_log.h"

static const char *TAG = "trace";

void kernel_trace_boot(const char *phase)
{
    ESP_LOGI(TAG, "[boot] %s", phase);
}

void kernel_trace_counter(const char *name, uint32_t value)
{
    ESP_LOGD(TAG, "[counter] %s=%" PRIu32, name, value);
}
