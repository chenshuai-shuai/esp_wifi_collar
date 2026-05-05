#include "platform_hal/log_control.h"

#include "esp_log.h"

static void set_level(const char *tag, esp_log_level_t level)
{
    esp_log_level_set(tag, level);
}

/*
 * Conversation-debug log policy.
 *
 * While we are still bringing up the real-time conversation feature we
 * silence every subsystem that is not directly useful for tracing the
 * ESP:CONV_START / ESP:CONV_STOP pipeline. Only the following tags are
 * allowed to print at INFO:
 *
 *   - app_mgr   (console command parser, mic uplink pump, audio mode)
 *   - conv_cli  (conversation_client: RPC, h2 stream, uplink counters)
 *
 * Everything else is pushed to WARN so noisy periodic subsystems (mic
 * health, audio status, supervisor, bsp, etc.) do not bury the
 * conversation diagnostics. Switch the Log Profile Kconfig to VERBOSE if
 * you need to see those logs again.
 */
void log_control_apply(void)
{
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
    /* Quiet + Normal both share the conversation-only profile for now. */
    esp_log_level_set("*",   ESP_LOG_WARN);
    set_level("main",        ESP_LOG_INFO);   /* FW-VER + boot identity */
    set_level("app_mgr",     ESP_LOG_INFO);
    set_level("conv_cli",    ESP_LOG_INFO);
    set_level("dlg_ul",      ESP_LOG_INFO);   /* uplink PCM probe / pacing diag */
    set_level("dlg_orch",    ESP_LOG_INFO);   /* dialog orchestrator: CONV_START/STOP, end-rpc */
    set_level("dlg_sess",    ESP_LOG_INFO);
    set_level("dlg_conn",    ESP_LOG_INFO);
    set_level("dlg_dl",      ESP_LOG_INFO);
    set_level("dlg_pb",      ESP_LOG_INFO);
    set_level("qemu_user",   ESP_LOG_INFO);   /* QEMU virtual user driving the loop */
    /*
     * Diagnostic helper: if conv_cli keeps showing wifi=0 we need to
     * see the wifi_svc state transitions (ssid scanning, auth, got ip,
     * retries, etc.) to tell AP-side issues apart from firmware-side.
     */
    set_level("wifi_svc",    ESP_LOG_INFO);
    set_level("wifi",        ESP_LOG_INFO);   /* IDF internal wifi driver */

    /* Keep these at WARN so they still surface real failures but don't
     * spam periodic status every second. */

    /*
     * Promoted to INFO under the QEMU bring-up: we want to see Wi-Fi state
     * transitions (service_mgr) and outbound TCP probe results (cloud_svc)
     * to debug the OpenETH path. Real-hardware behaviour stays unchanged
     * because these tags emit the same INFO lines on real Wi-Fi too.
     */
    set_level("cloud_svc",   ESP_LOG_INFO);
    set_level("service_mgr", ESP_LOG_INFO);
    set_level("kernel",      ESP_LOG_WARN);
    set_level("hal",         ESP_LOG_WARN);
    set_level("bsp",         ESP_LOG_WARN);
    set_level("mic",         ESP_LOG_WARN);
    set_level("supervisor",  ESP_LOG_WARN);
    set_level("trace",       ESP_LOG_WARN);
    set_level("httpd",       ESP_LOG_ERROR);
    set_level("httpd_uri",   ESP_LOG_ERROR);
    set_level("httpd_txrx",  ESP_LOG_ERROR);
    set_level("httpd_parse", ESP_LOG_ERROR);
#endif
}
