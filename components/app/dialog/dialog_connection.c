#include "dialog_connection.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "audio_dsp/audio_denoise.h"
#include "services/wifi_service.h"

static const char *TAG = "dlg_conn";

esp_err_t dialog_connection_prepare_for_start(void)
{
    (void)wifi_service_set_realtime_mode(true);

    const uint32_t heap_before = (uint32_t)esp_get_free_heap_size();
    const uint32_t largest_before =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (audio_denoise_is_ready()) {
        audio_denoise_deinit();
        ESP_LOGI(TAG,
                 "pre-start released denoise (free_heap %u->%u, largest %u->%u)",
                 (unsigned)heap_before,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)largest_before,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }

    return ESP_OK;
}

void dialog_connection_restore_idle_profile(void)
{
    (void)wifi_service_set_realtime_mode(false);
}

conversation_state_t dialog_connection_state(void)
{
    return conversation_client_get_state();
}

const char *dialog_connection_state_str(conversation_state_t state)
{
    return conversation_client_state_str(state);
}

bool dialog_connection_stream_writable(void)
{
    return conversation_client_stream_writable();
}
