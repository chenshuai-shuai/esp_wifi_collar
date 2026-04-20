#include "bsp/speaker_output.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "speaker";

static i2s_chan_handle_t s_tx_chan;
static bool s_speaker_ready;

static esp_err_t bsp_speaker_enable_amp(bool enabled)
{
#if !CONFIG_COLLAR_SPEAKER_ENABLE
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
#else
    const gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_COLLAR_SPEAKER_SD_MODE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&gpio_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    return gpio_set_level(CONFIG_COLLAR_SPEAKER_SD_MODE_GPIO, enabled ? 1 : 0);
#endif
}

esp_err_t bsp_speaker_init(void)
{
#if !CONFIG_COLLAR_SPEAKER_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_speaker_ready) {
        return ESP_OK;
    }

    esp_err_t ret = bsp_speaker_enable_amp(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to drive SD_MODE gpio=%d: %s",
                 CONFIG_COLLAR_SPEAKER_SD_MODE_GPIO, esp_err_to_name(ret));
        return ret;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S TX channel: %s", esp_err_to_name(ret));
        s_tx_chan = NULL;
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_COLLAR_SPEAKER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_COLLAR_SPEAKER_BCLK_GPIO,
            .ws = CONFIG_COLLAR_SPEAKER_LRCLK_GPIO,
            .dout = CONFIG_COLLAR_SPEAKER_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S STD mode: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    int16_t silence[64] = {0};
    size_t bytes_loaded = 0U;
    ret = i2s_channel_preload_data(s_tx_chan, silence, sizeof(silence), &bytes_loaded);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to preload I2S silence: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    s_speaker_ready = true;
    ESP_LOGI(TAG,
             "Speaker output ready: sd=%d dout=%d bclk=%d lrclk=%d rate=%d format=i2s stereo16",
             CONFIG_COLLAR_SPEAKER_SD_MODE_GPIO,
             CONFIG_COLLAR_SPEAKER_DOUT_GPIO,
             CONFIG_COLLAR_SPEAKER_BCLK_GPIO,
             CONFIG_COLLAR_SPEAKER_LRCLK_GPIO,
             CONFIG_COLLAR_SPEAKER_SAMPLE_RATE);
    return ESP_OK;
#endif
}

bool bsp_speaker_is_ready(void)
{
#if !CONFIG_COLLAR_SPEAKER_ENABLE
    return false;
#else
    return s_speaker_ready && s_tx_chan != NULL;
#endif
}

uint32_t bsp_speaker_get_sample_rate_hz(void)
{
#if !CONFIG_COLLAR_SPEAKER_ENABLE
    return 0U;
#else
    return (uint32_t)CONFIG_COLLAR_SPEAKER_SAMPLE_RATE;
#endif
}

esp_err_t bsp_speaker_write(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
#if !CONFIG_COLLAR_SPEAKER_ENABLE
    (void)data;
    (void)len;
    (void)bytes_written;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!bsp_speaker_is_ready() || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2s_channel_write(s_tx_chan, data, len, bytes_written, timeout_ms);
#endif
}
