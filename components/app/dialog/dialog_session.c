#include "dialog_session.h"

#include "esp_log.h"

static const char *TAG = "dlg_sess";

esp_err_t dialog_session_start(void)
{
    esp_err_t ret = conversation_client_start_conversation(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "start_conversation failed: 0x%x", ret);
        return ret;
    }

    ret = conversation_client_start_talking();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "start_talking failed: 0x%x", ret);
    }
    return ret;
}

esp_err_t dialog_session_stop_local(void)
{
    if (conversation_client_get_state() == CONVERSATION_STATE_IDLE) {
        return ESP_OK;
    }
    return conversation_client_end_conversation();
}

esp_err_t dialog_session_send_end_rpc(const char *session_id)
{
    return conversation_client_send_end_rpc(session_id);
}

bool dialog_session_is_idle(void)
{
    return conversation_client_get_state() == CONVERSATION_STATE_IDLE;
}

conversation_state_t dialog_session_state(void)
{
    return conversation_client_get_state();
}
