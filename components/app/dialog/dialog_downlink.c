#include "dialog_downlink.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#include "conversation/conversation_client.h"

#include "dialog_playback.h"

static const char *TAG = "dlg_dl";

static bool s_started;

static void dialog_downlink_on_audio_output(const uint8_t *pcm,
                                            size_t len,
                                            int64_t seq,
                                            void *arg)
{
    (void)arg;
    (void)seq;
    (void)dialog_playback_write(pcm, len);
}

static void dialog_downlink_on_audio_complete(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio_complete: played=%u", (unsigned)dialog_playback_frames_played());
}

static void dialog_downlink_on_audio_start(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio_start");
}

static void dialog_downlink_on_session_start(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "session_start");
}

static void dialog_downlink_on_error(const char *code, const char *message, void *arg)
{
    (void)arg;
    ESP_LOGW(TAG,
             "server ErrorEvent code='%s' msg='%s'",
             code ? code : "-",
             message ? message : "-");
}

esp_err_t dialog_downlink_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    conversation_client_set_audio_output_listener(dialog_downlink_on_audio_output, NULL);
    conversation_client_set_audio_complete_listener(dialog_downlink_on_audio_complete, NULL);
    conversation_client_set_audio_start_listener(dialog_downlink_on_audio_start, NULL);
    conversation_client_set_session_start_listener(dialog_downlink_on_session_start, NULL);
    conversation_client_set_error_listener(dialog_downlink_on_error, NULL);

    s_started = true;
    return ESP_OK;
}

void dialog_downlink_reset_turn(void)
{
    dialog_playback_reset_counters();
}
