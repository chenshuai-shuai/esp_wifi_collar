#include "platform_hal/log_control.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_uart_log_mutex_buf;
static SemaphoreHandle_t s_uart_log_mutex;
static vprintf_like_t s_prev_vprintf;

static void log_control_init_uart_lock(void)
{
    if (s_uart_log_mutex == NULL) {
        s_uart_log_mutex = xSemaphoreCreateMutexStatic(&s_uart_log_mutex_buf);
    }
}

void log_control_uart_lock(void)
{
    log_control_init_uart_lock();
    if (s_uart_log_mutex != NULL) {
        (void)xSemaphoreTake(s_uart_log_mutex, portMAX_DELAY);
    }
}

void log_control_uart_unlock(void)
{
    if (s_uart_log_mutex != NULL) {
        (void)xSemaphoreGive(s_uart_log_mutex);
    }
}

static int locked_vprintf(const char *fmt, va_list args)
{
    int ret;

    log_control_uart_lock();
    if (s_prev_vprintf != NULL) {
        ret = s_prev_vprintf(fmt, args);
    } else {
        ret = vprintf(fmt, args);
    }
    log_control_uart_unlock();

    return ret;
}

static void set_level(const char *tag, esp_log_level_t level)
{
    esp_log_level_set(tag, level);
}

/*
 * Handoff-debug log policy.
 *
 * During nRF <-> ESP32 owner-transfer bring-up, keep UART logs focused on
 * the handoff state machine. Periodic Wi-Fi/cloud/conversation status logs
 * are intentionally hidden unless they are warnings/errors.
 */
void log_control_apply(void)
{
    log_control_init_uart_lock();
    if (s_prev_vprintf == NULL) {
        s_prev_vprintf = esp_log_set_vprintf(locked_vprintf);
    }

#if CONFIG_COLLAR_LOG_PROFILE_VERBOSE
    /* Developer mode: show everything, plus debug on the conversation path. */
    esp_log_level_set("*", ESP_LOG_INFO);
    set_level("main",        ESP_LOG_INFO);   /* FW-VER + boot identity */
    set_level("app_mgr",     ESP_LOG_DEBUG);
    set_level("conv_cli",    ESP_LOG_DEBUG);

    set_level("service_mgr", ESP_LOG_INFO);
    set_level("supervisor",  ESP_LOG_DEBUG);
    set_level("trace",       ESP_LOG_DEBUG);
    set_level("httpd",       ESP_LOG_ERROR);
    set_level("httpd_uri",   ESP_LOG_ERROR);
    set_level("httpd_txrx",  ESP_LOG_ERROR);
    set_level("httpd_parse", ESP_LOG_ERROR);
#else
    /* Quiet + Normal both share the handoff-only profile for now. */
    esp_log_level_set("*",   ESP_LOG_WARN);
    set_level("main",        ESP_LOG_INFO);   /* FW-VER + boot identity */
    set_level("app_mgr",     ESP_LOG_INFO);   /* UART reader + owner-gated app loop */
    set_level("dlg_orch",    ESP_LOG_INFO);   /* dialog orchestrator: auto start/stop */
    set_level("audio_handoff", ESP_LOG_INFO);

    /* Keep these at WARN so they still surface real failures but don't
     * spam periodic status every second. */
    set_level("conv_cli",    ESP_LOG_WARN);
    set_level("dlg_ul",      ESP_LOG_WARN);
    set_level("dlg_sess",    ESP_LOG_WARN);
    set_level("dlg_conn",    ESP_LOG_WARN);
    set_level("dlg_dl",      ESP_LOG_WARN);
    set_level("dlg_pb",      ESP_LOG_WARN);
    set_level("qemu_user",   ESP_LOG_WARN);
    set_level("wifi_svc",    ESP_LOG_WARN);
    set_level("wifi",        ESP_LOG_WARN);
    set_level("cloud_svc",   ESP_LOG_WARN);
    set_level("service_mgr", ESP_LOG_WARN);
    set_level("kernel",      ESP_LOG_WARN);
    set_level("hal",         ESP_LOG_WARN);
    set_level("bsp",         ESP_LOG_WARN);
    set_level("mic",         ESP_LOG_WARN);
    set_level("speaker",     ESP_LOG_WARN);
    set_level("supervisor",  ESP_LOG_WARN);
    set_level("trace",       ESP_LOG_WARN);
    set_level("httpd",       ESP_LOG_ERROR);
    set_level("httpd_uri",   ESP_LOG_ERROR);
    set_level("httpd_txrx",  ESP_LOG_ERROR);
    set_level("httpd_parse", ESP_LOG_ERROR);
#endif
}
