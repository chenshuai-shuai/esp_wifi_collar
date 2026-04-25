#include "dialog_playback.h"

#include "esp_log.h"

#include "bsp/speaker_output.h"

static const char *TAG = "dlg_play";

static uint32_t s_frames_played;

esp_err_t dialog_playback_write(const uint8_t *pcm, size_t len)
{
    if (pcm == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_speaker_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t written = 0U;
    esp_err_t ret = bsp_speaker_write(pcm, len, &written, 60U);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "speaker write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_frames_played++;
    return ESP_OK;
}

void dialog_playback_reset_counters(void)
{
    s_frames_played = 0U;
}

uint32_t dialog_playback_frames_played(void)
{
    return s_frames_played;
}
