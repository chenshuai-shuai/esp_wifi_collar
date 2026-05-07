#include "app/audio_handoff.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "audio_dsp/audio_denoise.h"
#include "app/dialog_orchestrator.h"
#include "bsp/bsp_board.h"
#include "bsp/microphone_input.h"
#include "bsp/speaker_output.h"
#include "platform_hal/log_control.h"
#include "services/wifi_service.h"

#define AUDIO_HANDOFF_RELEASE_QUIESCE_MS 100U
#define AUDIO_HANDOFF_TAKE_GUARD_MS      10U
#define AUDIO_HANDOFF_READY_TX_COUNT     3U
#define AUDIO_HANDOFF_READY_TX_GAP_MS    200U
#define AUDIO_HANDOFF_READY_STACK_WORDS  2048
/* Bring-up switches. Keep both disabled for real ESP owner handoff:
 * ACK is returned only after the requested hardware transition completes. */
#define AUDIO_HANDOFF_TAKE_ACK_ONLY      0
#define AUDIO_HANDOFF_RELEASE_ACK_ONLY   0
#define AUDIO_HANDOFF_TAKE_MIC_ONLY      1

typedef enum {
    AUDIO_HANDOFF_SAFE_HANDOFF = 0,
    AUDIO_HANDOFF_ESP_AUDIO_OWNER,
    AUDIO_HANDOFF_FAULT,
} audio_handoff_state_t;

static const char *TAG = "audio_handoff";

static volatile audio_handoff_state_t s_state = AUDIO_HANDOFF_SAFE_HANDOFF;
static char s_fault_reason[32] = "NOT_READY";
static bool s_initialized;
static volatile bool s_link_established;
static volatile audio_handoff_state_t s_dry_run_state = AUDIO_HANDOFF_SAFE_HANDOFF;
static bool s_ready_task_started;
static StaticTask_t s_ready_tcb;
static StackType_t s_ready_stack[AUDIO_HANDOFF_READY_STACK_WORDS];

static void audio_handoff_log_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "heap[%s]: internal=%u largest_internal=%u dma=%u largest_dma=%u min_free=%u",
             stage,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void audio_handoff_set_fault(const char *reason)
{
    if (reason == NULL || reason[0] == '\0') {
        reason = "INVALID_STATE";
    }

    strlcpy(s_fault_reason, reason, sizeof(s_fault_reason));
    s_state = AUDIO_HANDOFF_FAULT;
}

static bool audio_handoff_download_mode_active(void)
{
    /* If this firmware is executing, ROM download mode is not active.
     * Keep this as a separate hook so a future BOOT/EN detector can be
     * wired in without changing protocol handling. */
    return false;
}

static void audio_handoff_reply(const char *line)
{
    if (line == NULL) {
        return;
    }

    const uart_port_t port = UART_NUM_0;
    log_control_uart_lock();
    (void)uart_write_bytes(port, line, strlen(line));
    (void)uart_write_bytes(port, "\r\n", 2);
    (void)uart_wait_tx_done(port, pdMS_TO_TICKS(100));
    log_control_uart_unlock();
    ESP_LOGI(TAG, "TX: %s", line);
}

static bool audio_handoff_is_runtime_command(const char *line)
{
    return strcmp(line, "ESP_PING") == 0 ||
           strcmp(line, "STATE?") == 0 ||
           strcmp(line, "REQ_AUDIO_RELEASE") == 0 ||
           strcmp(line, "REQ_AUDIO_TAKE") == 0;
}

static bool audio_handoff_require_link(const char *line)
{
    if (s_link_established) {
        return true;
    }

    ESP_LOGW(TAG, "RX before NRF_READY_ACK ignored: %s", line);
    return false;
}

static void audio_handoff_ready_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(100));

    for (uint32_t i = 0; i < AUDIO_HANDOFF_READY_TX_COUNT; ++i) {
        if (s_link_established) {
            break;
        }

        audio_handoff_reply("ESP_READY");
        vTaskDelay(pdMS_TO_TICKS(AUDIO_HANDOFF_READY_TX_GAP_MS));
    }

    if (!s_link_established) {
        ESP_LOGI(TAG, "waiting for NRF_READY_ACK");
    }

    vTaskDelete(NULL);
}

static char *audio_handoff_trim(char *line)
{
    if (line == NULL) {
        return NULL;
    }

    while (*line == ' ' || *line == '\t') {
        line++;
    }

    size_t n = strlen(line);
    while (n > 0U && (line[n - 1U] == '\r' || line[n - 1U] == '\n' ||
                      line[n - 1U] == ' ' || line[n - 1U] == '\t')) {
        line[--n] = '\0';
    }

    return line;
}

esp_err_t audio_handoff_init(void)
{
    esp_err_t ret = bsp_audio_shared_gpio_highz();
    if (ret != ESP_OK) {
        audio_handoff_set_fault("GPIO_CONFIG_FAIL");
        ESP_LOGE(TAG, "init high-z failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = AUDIO_HANDOFF_SAFE_HANDOFF;
    s_dry_run_state = AUDIO_HANDOFF_SAFE_HANDOFF;
    s_fault_reason[0] = '\0';
    s_link_established = false;
    s_initialized = true;
    ESP_LOGI(TAG, "ready: state=SAFE_HANDOFF shared_audio_gpio=high-z");
    return ESP_OK;
}

esp_err_t audio_handoff_uart_ready(void)
{
    ESP_LOGI(TAG, "UART ready");

    if (s_ready_task_started) {
        return ESP_OK;
    }

    TaskHandle_t task = xTaskCreateStatic(
        audio_handoff_ready_task,
        "audio_ready_tx",
        AUDIO_HANDOFF_READY_STACK_WORDS,
        NULL,
        3,
        s_ready_stack,
        &s_ready_tcb);
    if (task == NULL) {
        return ESP_FAIL;
    }

    s_ready_task_started = true;
    return ESP_OK;
}

esp_err_t audio_handoff_enter_safe(void)
{
    if (!s_initialized) {
        esp_err_t ret = audio_handoff_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    s_state = AUDIO_HANDOFF_SAFE_HANDOFF;

    audio_handoff_log_heap("release-begin");
    ESP_LOGI(TAG, "audio release: stopping local audio path");
    (void)wifi_service_set_realtime_mode(false);
    dialog_orchestrator_abort_local("audio handoff release");
    vTaskDelay(pdMS_TO_TICKS(AUDIO_HANDOFF_RELEASE_QUIESCE_MS));

    audio_denoise_deinit();
    bsp_microphone_deinit();
    bsp_speaker_deinit();
    ESP_LOGI(TAG, "audio peripheral stopped");
    audio_handoff_log_heap("after-audio-stop");

    esp_err_t ret = bsp_audio_shared_gpio_highz();
    if (ret != ESP_OK) {
        audio_handoff_set_fault("GPIO_CONFIG_FAIL");
        ESP_LOGE(TAG, "enter safe high-z failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "shared_audio_gpio -> high-z");
    audio_handoff_log_heap("release-done");

    s_state = AUDIO_HANDOFF_SAFE_HANDOFF;
    s_fault_reason[0] = '\0';
    ESP_LOGI(TAG, "state -> SAFE_HANDOFF");
    return ESP_OK;
}

esp_err_t audio_handoff_enter_esp_owner(void)
{
    if (!s_initialized) {
        esp_err_t ret = audio_handoff_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (audio_handoff_download_mode_active()) {
        audio_handoff_set_fault("DOWNLOAD_MODE_ACTIVE");
        return ESP_ERR_INVALID_STATE;
    }

    audio_handoff_log_heap("take-begin");
    esp_err_t ret = bsp_audio_shared_gpio_highz();
    if (ret != ESP_OK) {
        audio_handoff_set_fault("GPIO_CONFIG_FAIL");
        ESP_LOGE(TAG, "pre-take high-z failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(AUDIO_HANDOFF_TAKE_GUARD_MS));
    ESP_LOGI(TAG, "shared_audio_gpio -> active");
    audio_handoff_log_heap("after-shared-gpio-active");

#if CONFIG_COLLAR_SPEAKER_ENABLE && !AUDIO_HANDOFF_TAKE_MIC_ONLY
    ESP_LOGI(TAG, "speaker init begin");
    audio_handoff_log_heap("before-speaker-init");
    ret = bsp_speaker_init();
    if (ret != ESP_OK) {
        audio_handoff_set_fault("AUDIO_START_FAIL");
        bsp_speaker_deinit();
        (void)bsp_audio_shared_gpio_highz();
        ESP_LOGE(TAG, "speaker init failed during take: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "speaker init done");
    audio_handoff_log_heap("after-speaker-init");
#elif CONFIG_COLLAR_SPEAKER_ENABLE
    ESP_LOGW(TAG, "speaker init skipped: mic-only handoff bring-up");
#endif

#if CONFIG_COLLAR_MICROPHONE_ENABLE
    ESP_LOGI(TAG, "microphone init begin");
    audio_handoff_log_heap("before-microphone-init");
    ret = bsp_microphone_init();
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "microphone not supported on this target; take continues");
    } else if (ret != ESP_OK) {
        audio_handoff_set_fault("AUDIO_START_FAIL");
        bsp_speaker_deinit();
        (void)bsp_audio_shared_gpio_highz();
        ESP_LOGE(TAG, "microphone init failed during take: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "microphone init done");
    audio_handoff_log_heap("after-microphone-init");
#endif

    ESP_LOGI(TAG, "audio peripheral started");
    audio_handoff_log_heap("take-done");
    s_state = AUDIO_HANDOFF_ESP_AUDIO_OWNER;
    s_fault_reason[0] = '\0';
    ESP_LOGI(TAG, "state -> ESP_AUDIO_OWNER");
    return ESP_OK;
}

bool audio_handoff_is_esp_owner(void)
{
    return s_state == AUDIO_HANDOFF_ESP_AUDIO_OWNER;
}

const char *audio_handoff_state_string(void)
{
    switch (s_state) {
    case AUDIO_HANDOFF_SAFE_HANDOFF:
        return "SAFE_HANDOFF";
    case AUDIO_HANDOFF_ESP_AUDIO_OWNER:
        return "ESP_AUDIO_OWNER";
    case AUDIO_HANDOFF_FAULT:
    default:
        return "FAULT";
    }
}

bool audio_handoff_process_line(char *line)
{
    line = audio_handoff_trim(line);
    if (line == NULL || line[0] == '\0') {
        return false;
    }

    if (strcmp(line, "NRF_READY_ACK") == 0) {
        ESP_LOGI(TAG, "RX: NRF_READY_ACK");
        s_link_established = true;
        return true;
    }

    if (audio_handoff_is_runtime_command(line)) {
        ESP_LOGI(TAG, "RX: %s", line);
        if (!audio_handoff_require_link(line)) {
            return true;
        }
    }

    if (strcmp(line, "ESP_PING") == 0) {
        audio_handoff_reply("ESP_PONG");
        return true;
    }

    if (strcmp(line, "STATE?") == 0) {
        char reply[64];
        if (s_state == AUDIO_HANDOFF_FAULT) {
            snprintf(reply, sizeof(reply), "STATE:FAULT:%s",
                     s_fault_reason[0] != '\0' ? s_fault_reason : "INVALID_STATE");
        } else {
#if AUDIO_HANDOFF_TAKE_ACK_ONLY || AUDIO_HANDOFF_RELEASE_ACK_ONLY
            const audio_handoff_state_t report_state = s_dry_run_state;
            const char *state_str = report_state == AUDIO_HANDOFF_ESP_AUDIO_OWNER
                                        ? "ESP_AUDIO_OWNER"
                                        : "SAFE_HANDOFF";
            snprintf(reply, sizeof(reply), "STATE:%s", state_str);
#else
            snprintf(reply, sizeof(reply), "STATE:%s", audio_handoff_state_string());
#endif
        }
        audio_handoff_reply(reply);
        return true;
    }

    if (strcmp(line, "REQ_AUDIO_RELEASE") == 0) {
#if AUDIO_HANDOFF_RELEASE_ACK_ONLY
        ESP_LOGW(TAG, "REQ_AUDIO_RELEASE dry-run: ACK only, audio GPIO/peripherals untouched");
        s_dry_run_state = AUDIO_HANDOFF_SAFE_HANDOFF;
        audio_handoff_reply("ACK_AUDIO_RELEASE");
        return true;
#else
        esp_err_t ret = audio_handoff_enter_safe();
        if (ret == ESP_OK) {
            audio_handoff_reply("ACK_AUDIO_RELEASE");
        } else {
            audio_handoff_reply("FAULT:AUDIO_RELEASE_FAIL");
        }
        return true;
#endif
    }

    if (strcmp(line, "REQ_AUDIO_TAKE") == 0) {
#if AUDIO_HANDOFF_TAKE_ACK_ONLY
        ESP_LOGW(TAG, "REQ_AUDIO_TAKE dry-run: ACK only, audio GPIO/peripherals untouched");
        s_dry_run_state = AUDIO_HANDOFF_ESP_AUDIO_OWNER;
        audio_handoff_reply("ACK_AUDIO_TAKE");
        return true;
#else
        esp_err_t ret = audio_handoff_enter_esp_owner();
        if (ret == ESP_OK) {
            audio_handoff_reply("ACK_AUDIO_TAKE");
            dialog_orchestrator_set_auto_enabled(true, "audio handoff take");
        } else if (strcmp(s_fault_reason, "DOWNLOAD_MODE_ACTIVE") == 0) {
            audio_handoff_reply("FAULT:DOWNLOAD_MODE_ACTIVE");
        } else if (strcmp(s_fault_reason, "GPIO_CONFIG_FAIL") == 0) {
            audio_handoff_reply("FAULT:GPIO_CONFIG_FAIL");
        } else {
            audio_handoff_reply("FAULT:AUDIO_START_FAIL");
        }
        return true;
#endif
    }

    return false;
}
