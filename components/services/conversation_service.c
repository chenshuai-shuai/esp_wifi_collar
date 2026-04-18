#include "services/conversation_service.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "services/cloud_service.h"

#define CONVERSATION_SERVICE_STACK_WORDS  2048
#define CONVERSATION_SERVICE_PRIORITY     8
#define CONVERSATION_HOST_MAX_LEN         128
#define CONVERSATION_USER_ID_MAX_LEN      64
#define CONVERSATION_ENCODING_MAX_LEN     16

static const char *TAG = "conv_svc";

typedef struct {
    bool started;
    bool wifi_ready;
    bool configured;
    uint16_t port;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bit_depth;
    char host[CONVERSATION_HOST_MAX_LEN];
    char user_id[CONVERSATION_USER_ID_MAX_LEN];
    char encoding[CONVERSATION_ENCODING_MAX_LEN];
} conversation_service_state_t;

static conversation_service_state_t s_conversation;
static StaticTask_t s_conversation_tcb;
static StackType_t s_conversation_stack[CONVERSATION_SERVICE_STACK_WORDS];

static const char *conversation_uplink_base64_string(void)
{
#ifdef CONFIG_COLLAR_CONVERSATION_UPLINK_BASE64
    return "on";
#else
    return "off";
#endif
}

static const char *conversation_downlink_base64_string(void)
{
#ifdef CONFIG_COLLAR_CONVERSATION_DOWNLINK_BASE64
    return "on";
#else
    return "off";
#endif
}

static const char *conversation_autodetect_base64_string(void)
{
#ifdef CONFIG_COLLAR_CONVERSATION_AUTODETECT_DOWNLINK_BASE64
    return "on";
#else
    return "off";
#endif
}

static const char *conversation_service_state_string(void)
{
    if (!CONFIG_COLLAR_CONVERSATION_ENABLE) {
        return "disabled";
    }

    if (!s_conversation.configured) {
        return "unconfigured";
    }

    if (!s_conversation.wifi_ready) {
        return "wait_wifi";
    }

    if (cloud_service_is_reachable() &&
        strcmp(cloud_service_host(), s_conversation.host) == 0 &&
        cloud_service_port() == s_conversation.port) {
        return "tcp_ready";
    }

    if (cloud_service_is_reachable()) {
        return "probe_mismatch";
    }

    if (cloud_service_has_attempted_probe() &&
        strcmp(cloud_service_host(), s_conversation.host) == 0 &&
        cloud_service_port() == s_conversation.port) {
        return "tcp_failed";
    }

    return "wait_tcp_probe";
}

static void conversation_service_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t conversation_service_start(void)
{
    if (s_conversation.started) {
        return ESP_OK;
    }

    memset(&s_conversation, 0, sizeof(s_conversation));
    strlcpy(s_conversation.host, CONFIG_COLLAR_CONVERSATION_HOST, sizeof(s_conversation.host));
    strlcpy(s_conversation.user_id, CONFIG_COLLAR_CONVERSATION_USER_ID, sizeof(s_conversation.user_id));
    strlcpy(s_conversation.encoding, CONFIG_COLLAR_CONVERSATION_ENCODING, sizeof(s_conversation.encoding));
    s_conversation.port = (uint16_t)CONFIG_COLLAR_CONVERSATION_PORT;
    s_conversation.sample_rate = (uint32_t)CONFIG_COLLAR_CONVERSATION_AUDIO_SAMPLE_RATE;
    s_conversation.channels = (uint8_t)CONFIG_COLLAR_CONVERSATION_AUDIO_CHANNELS;
    s_conversation.bit_depth = (uint8_t)CONFIG_COLLAR_CONVERSATION_AUDIO_BIT_DEPTH;
    s_conversation.configured = CONFIG_COLLAR_CONVERSATION_ENABLE && s_conversation.host[0] != '\0';

    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        conversation_service_task,
        "conv_svc",
        CONVERSATION_SERVICE_STACK_WORDS,
        NULL,
        CONVERSATION_SERVICE_PRIORITY,
        s_conversation_stack,
        &s_conversation_tcb,
        tskNO_AFFINITY);
    if (task_handle == NULL) {
        return ESP_FAIL;
    }

    s_conversation.started = true;

    if (!CONFIG_COLLAR_CONVERSATION_ENABLE) {
        ESP_LOGI(TAG, "Conversation service disabled by config");
    } else if (!s_conversation.configured) {
        ESP_LOGI(TAG, "Conversation service idle: host not configured");
    } else {
        ESP_LOGI(TAG,
                 "Conversation target armed: host=%s port=%u user=%s audio=%lu/%u/%u enc=%s up_b64=%s down_b64=%s auto_b64=%s",
                 s_conversation.host,
                 (unsigned int)s_conversation.port,
                 s_conversation.user_id,
                 (unsigned long)s_conversation.sample_rate,
                 (unsigned int)s_conversation.channels,
                 (unsigned int)s_conversation.bit_depth,
                 s_conversation.encoding,
                 conversation_uplink_base64_string(),
                 conversation_downlink_base64_string(),
                 conversation_autodetect_base64_string());
    }

    return ESP_OK;
}

void conversation_service_handle_wifi_state(kernel_wifi_state_t state)
{
    switch (state) {
    case KERNEL_WIFI_STATE_GOT_IP:
        s_conversation.wifi_ready = true;
        break;

    case KERNEL_WIFI_STATE_PROVISIONING:
    case KERNEL_WIFI_STATE_CONNECTING:
    case KERNEL_WIFI_STATE_DISCONNECTED:
    case KERNEL_WIFI_STATE_FAILED:
        s_conversation.wifi_ready = false;
        break;

    default:
        break;
    }
}

void conversation_service_log_status(void)
{
    const char *probe_error = cloud_service_last_error();

    ESP_LOGI(TAG,
             "Conversation status: state=%s host=%s port=%u user=%s wifi=%s cloud=%s probe_err=%s audio=%lu/%u/%u enc=%s up_b64=%s down_b64=%s auto_b64=%s",
             conversation_service_state_string(),
             s_conversation.host[0] != '\0' ? s_conversation.host : "-",
             (unsigned int)s_conversation.port,
             s_conversation.user_id[0] != '\0' ? s_conversation.user_id : "-",
             s_conversation.wifi_ready ? "ready" : "no",
             cloud_service_is_reachable() ? "reachable" : "no",
             probe_error[0] != '\0' ? probe_error : "-",
             (unsigned long)s_conversation.sample_rate,
             (unsigned int)s_conversation.channels,
             (unsigned int)s_conversation.bit_depth,
             s_conversation.encoding[0] != '\0' ? s_conversation.encoding : "-",
             conversation_uplink_base64_string(),
             conversation_downlink_base64_string(),
             conversation_autodetect_base64_string());
}

bool conversation_service_is_configured(void)
{
    return s_conversation.configured;
}

const char *conversation_service_host(void)
{
    return s_conversation.host;
}

uint16_t conversation_service_port(void)
{
    return s_conversation.port;
}
