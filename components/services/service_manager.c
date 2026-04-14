#include "services/service_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "kernel/kernel_msgbus.h"
#include "kernel/kernel_trace.h"
#include "kernel/kernel_workqueue.h"
#include "services/wifi_service.h"

#define SERVICE_MANAGER_STACK_WORDS    2048
#define SERVICE_MANAGER_PRIORITY       9
#define SERVICE_MANAGER_CORE           tskNO_AFFINITY

static const char *TAG = "service_mgr";

static StaticTask_t s_service_tcb;
static StackType_t s_service_stack[SERVICE_MANAGER_STACK_WORDS];
static bool s_service_started;

static const char *wifi_state_to_string(kernel_wifi_state_t state)
{
    switch (state) {
    case KERNEL_WIFI_STATE_IDLE:
        return "idle";
    case KERNEL_WIFI_STATE_STARTING:
        return "starting";
    case KERNEL_WIFI_STATE_PROVISIONING:
        return "provisioning";
    case KERNEL_WIFI_STATE_CONNECTING:
        return "connecting";
    case KERNEL_WIFI_STATE_CONNECTED:
        return "connected";
    case KERNEL_WIFI_STATE_GOT_IP:
        return "got_ip";
    case KERNEL_WIFI_STATE_DISCONNECTED:
        return "disconnected";
    case KERNEL_WIFI_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static void service_housekeeping(void *ctx)
{
    (void)ctx;
    ESP_LOGD(TAG, "Deferred housekeeping finished");
}

static void service_manager_task(void *arg)
{
    (void)arg;

    kernel_msg_t msg;

    for (;;) {
        if (kernel_msgbus_receive(&msg, portMAX_DELAY) != pdPASS) {
            continue;
        }

        switch (msg.topic) {
        case KERNEL_TOPIC_SYSTEM_BOOT:
            ESP_LOGI(TAG, "Boot event received at %lld us", msg.timestamp_us);
            break;

        case KERNEL_TOPIC_RT_CYCLE:
            ESP_LOGD(TAG, "RT plane heartbeat cycles=%lu", (unsigned long)msg.value);
            break;

        case KERNEL_TOPIC_APP_HEARTBEAT:
            ESP_LOGD(TAG, "App heartbeat seq=%lu", (unsigned long)msg.value);
            if ((msg.value % 5U) == 0U) {
                const kernel_work_item_t item = {
                    .fn = service_housekeeping,
                    .ctx = NULL,
                };
                (void)kernel_workqueue_post(&item, 0);
            }
            break;

        case KERNEL_TOPIC_SERVICE_HEALTH:
            kernel_trace_counter("service_health_heap", msg.value);
            break;

        case KERNEL_TOPIC_WIFI_STATE:
            ESP_LOGI(TAG, "Wi-Fi state=%s", wifi_state_to_string((kernel_wifi_state_t)msg.value));
            break;

        default:
            ESP_LOGW(TAG, "Unhandled topic=%d", msg.topic);
            break;
        }
    }
}

esp_err_t service_manager_init(void)
{
    if (s_service_started) {
        return ESP_OK;
    }

    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        service_manager_task,
        "service_mgr",
        SERVICE_MANAGER_STACK_WORDS,
        NULL,
        SERVICE_MANAGER_PRIORITY,
        s_service_stack,
        &s_service_tcb,
        SERVICE_MANAGER_CORE
    );

    if (task_handle == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = wifi_service_start();
    if (ret != ESP_OK) {
        return ret;
    }

    s_service_started = true;
    return ESP_OK;
}
