#include "bsp/speaker_output.h"

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "speaker";

#define SPEAKER_BYTES_PER_FRAME                4U
#define SPEAKER_POSTPROC_CHUNK_BYTES           512U
#define SPEAKER_HPF_R_Q15                      32440
#define SPEAKER_OUTPUT_HEADROOM_PCT            82
#define SPEAKER_SOFT_LIMIT_THRESHOLD           18000
#define SPEAKER_SOFT_LIMIT_FULL_SCALE          30000

typedef struct {
    int32_t x_prev[2];
    int32_t y_prev[2];
    uint8_t scratch[SPEAKER_POSTPROC_CHUNK_BYTES];
} speaker_postproc_state_t;

static i2s_chan_handle_t s_tx_chan;
static bool s_speaker_ready;
static speaker_postproc_state_t s_postproc;

static int16_t bsp_speaker_soft_limit(int32_t sample)
{
    int32_t abs_sample = sample < 0 ? -sample : sample;
    if (abs_sample <= SPEAKER_SOFT_LIMIT_THRESHOLD) {
        return (int16_t)sample;
    }

    const int32_t sign = sample < 0 ? -1 : 1;
    const int32_t over = abs_sample - SPEAKER_SOFT_LIMIT_THRESHOLD;
    const int32_t soft_range = SPEAKER_SOFT_LIMIT_FULL_SCALE - SPEAKER_SOFT_LIMIT_THRESHOLD;
    int32_t limited =
        SPEAKER_SOFT_LIMIT_THRESHOLD + ((over * soft_range) / (soft_range + over));
    if (limited > 32767) {
        limited = 32767;
    }

    return (int16_t)(sign * limited);
}

static size_t bsp_speaker_postprocess_chunk(const uint8_t *src, size_t len, uint8_t *dst)
{
    if (src == NULL || dst == NULL || len == 0U || (len % SPEAKER_BYTES_PER_FRAME) != 0U) {
        return 0U;
    }

    const size_t frame_count = len / SPEAKER_BYTES_PER_FRAME;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        for (size_t ch = 0; ch < 2U; ++ch) {
            const size_t byte_index = (frame * SPEAKER_BYTES_PER_FRAME) + (ch * 2U);
            int16_t input = (int16_t)((uint16_t)src[byte_index] |
                                      ((uint16_t)src[byte_index + 1U] << 8));

            /* First-order DC blocker / gentle high-pass to reduce low-frequency mud. */
            int32_t hp = (int32_t)input - s_postproc.x_prev[ch] +
                         ((s_postproc.y_prev[ch] * SPEAKER_HPF_R_Q15) >> 15);
            s_postproc.x_prev[ch] = input;
            s_postproc.y_prev[ch] = hp;

            /* Reserve headroom because the analog gain is already high (15 dB). */
            hp = (hp * SPEAKER_OUTPUT_HEADROOM_PCT) / 100;

            if (hp > 32767) {
                hp = 32767;
            } else if (hp < -32768) {
                hp = -32768;
            }

            const int16_t output = bsp_speaker_soft_limit(hp);
            dst[byte_index] = (uint8_t)(output & 0xff);
            dst[byte_index + 1U] = (uint8_t)((output >> 8) & 0xff);
        }
    }

    return len;
}

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

    memset(&s_postproc, 0, sizeof(s_postproc));
    s_speaker_ready = true;
    ESP_LOGI(TAG,
             "Speaker output ready: sd=%d dout=%d bclk=%d lrclk=%d rate=%d format=i2s stereo16 post=hpf+softlimit headroom=%d%%",
             CONFIG_COLLAR_SPEAKER_SD_MODE_GPIO,
             CONFIG_COLLAR_SPEAKER_DOUT_GPIO,
             CONFIG_COLLAR_SPEAKER_BCLK_GPIO,
             CONFIG_COLLAR_SPEAKER_LRCLK_GPIO,
             CONFIG_COLLAR_SPEAKER_SAMPLE_RATE,
             SPEAKER_OUTPUT_HEADROOM_PCT);
    return ESP_OK;
#endif
}

void bsp_speaker_deinit(void)
{
#if !CONFIG_COLLAR_SPEAKER_ENABLE
    return;
#else
    if (s_tx_chan != NULL) {
        (void)i2s_channel_disable(s_tx_chan);
        (void)i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
    }
    (void)bsp_speaker_enable_amp(false);
    s_speaker_ready = false;
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

    if ((len % SPEAKER_BYTES_PER_FRAME) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total_written = 0U;
    const uint8_t *src = (const uint8_t *)data;
    while (total_written < len) {
        size_t chunk_len = len - total_written;
        if (chunk_len > sizeof(s_postproc.scratch)) {
            chunk_len = sizeof(s_postproc.scratch);
        }
        chunk_len = (chunk_len / SPEAKER_BYTES_PER_FRAME) * SPEAKER_BYTES_PER_FRAME;
        if (chunk_len == 0U) {
            break;
        }

        size_t processed_len = bsp_speaker_postprocess_chunk(
            src + total_written, chunk_len, s_postproc.scratch);
        if (processed_len == 0U) {
            break;
        }

        size_t chunk_written = 0U;
        esp_err_t ret = i2s_channel_write(
            s_tx_chan, s_postproc.scratch, processed_len, &chunk_written, timeout_ms);
        total_written += chunk_written;
        if (ret != ESP_OK) {
            if (bytes_written != NULL) {
                *bytes_written = total_written;
            }
            return ret;
        }
        if (chunk_written != processed_len) {
            break;
        }
    }

    if (bytes_written != NULL) {
        *bytes_written = total_written;
    }
    return total_written == len ? ESP_OK : ESP_ERR_TIMEOUT;
#endif
}
