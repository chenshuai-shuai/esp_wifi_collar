#include "app/dialog_orchestrator.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "dialog_connection.h"
#include "dialog_downlink.h"
#include "dialog_session.h"
#include "dialog_uplink.h"
#include "services/wifi_service.h"

#define DIALOG_AUTO_RETRY_MS         5000U
#define DIALOG_AUTO_WAIT_LOG_MS      5000U

static const char *TAG = "dlg_orch";

static bool s_started;
static bool s_active;
static bool s_auto_enabled;
static int64_t s_auto_next_retry_us;
static int64_t s_auto_last_wait_log_us;

static esp_err_t dialog_start_session(const char *source)
{
    const conversation_state_t st = dialog_session_state();
    if (st != CONVERSATION_STATE_IDLE) {
        ESP_LOGW(TAG,
                 "%s start ignored, current state=%s",
                 source,
                 dialog_connection_state_str(st));
        s_active = true;
        dialog_uplink_set_active(true);
        return ESP_OK;
    }

    ESP_LOGI(TAG, ">>> %s start", source);
    (void)dialog_connection_prepare_for_start();

    dialog_uplink_reset_turn();
    dialog_downlink_reset_turn();
    dialog_uplink_drain_stale();

    esp_err_t ret = dialog_session_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s start failed: 0x%x", source, ret);
        dialog_connection_restore_idle_profile();
        s_active = false;
        dialog_uplink_set_active(false);
        return ret;
    }

    s_active = true;
    dialog_uplink_set_active(true);
    return ESP_OK;
}

esp_err_t dialog_orchestrator_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(dialog_downlink_start());

    esp_err_t ret = dialog_uplink_start();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "orchestrator ready (uplink chunk=%u B)",
             (unsigned)dialog_uplink_chunk_bytes());

    s_started = true;
    return ESP_OK;
}

void dialog_orchestrator_set_auto_enabled(bool enabled, const char *reason)
{
    if (s_auto_enabled == enabled) {
        return;
    }

    s_auto_enabled = enabled;
    s_auto_next_retry_us = 0;
    s_auto_last_wait_log_us = 0;
    ESP_LOGI(TAG, "auto conversation %s%s%s",
             enabled ? "enabled" : "disabled",
             reason != NULL ? ": " : "",
             reason != NULL ? reason : "");

    if (!enabled) {
        s_active = false;
        dialog_uplink_set_active(false);
    }
}

void dialog_orchestrator_auto_tick(int64_t now_us)
{
    if (!s_auto_enabled || !s_started) {
        return;
    }

    if (s_active || dialog_session_state() != CONVERSATION_STATE_IDLE) {
        return;
    }

    if (!wifi_service_sta_ready()) {
        if (s_auto_last_wait_log_us == 0 ||
            (now_us - s_auto_last_wait_log_us) >= ((int64_t)DIALOG_AUTO_WAIT_LOG_MS * 1000LL)) {
            ESP_LOGI(TAG, "auto conversation waiting for Wi-Fi/IP");
            s_auto_last_wait_log_us = now_us;
        }
        return;
    }

    if (now_us < s_auto_next_retry_us) {
        return;
    }

    esp_err_t ret = dialog_start_session("auto");
    if (ret != ESP_OK) {
        s_auto_next_retry_us = now_us + ((int64_t)DIALOG_AUTO_RETRY_MS * 1000LL);
        return;
    }

    s_auto_next_retry_us = 0;
}

void dialog_orchestrator_abort_local(const char *reason)
{
    ESP_LOGI(TAG, "abort local%s%s",
             reason != NULL ? ": " : "",
             reason != NULL ? reason : "");

    s_active = false;
    s_auto_enabled = false;
    s_auto_next_retry_us = 0;
    s_auto_last_wait_log_us = 0;
    dialog_uplink_set_active(false);
    dialog_connection_restore_idle_profile();
    (void)dialog_session_stop_local();
}

bool dialog_orchestrator_is_active(void)
{
    return s_active;
}
