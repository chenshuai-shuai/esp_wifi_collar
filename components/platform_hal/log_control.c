#include "platform_hal/log_control.h"

#include "esp_log.h"

static void set_level(const char *tag, esp_log_level_t level)
{
    esp_log_level_set(tag, level);
}

void log_control_apply(void)
{
#if CONFIG_COLLAR_LOG_PROFILE_QUIET
    esp_log_level_set("*", ESP_LOG_WARN);
    set_level("main", ESP_LOG_INFO);
    set_level("bsp", ESP_LOG_INFO);
    set_level("hal", ESP_LOG_INFO);
    set_level("kernel", ESP_LOG_INFO);
    set_level("wifi_svc", ESP_LOG_INFO);
    set_level("cloud_svc", ESP_LOG_INFO);
    set_level("conv_svc", ESP_LOG_INFO);
    set_level("service_mgr", ESP_LOG_INFO);
    set_level("app_mgr", ESP_LOG_WARN);
    set_level("supervisor", ESP_LOG_WARN);
    set_level("trace", CONFIG_COLLAR_ENABLE_VERBOSE_BOOT_LOG ? ESP_LOG_INFO : ESP_LOG_WARN);
    set_level("httpd", ESP_LOG_ERROR);
    set_level("httpd_uri", ESP_LOG_ERROR);
    set_level("httpd_txrx", ESP_LOG_ERROR);
    set_level("httpd_parse", ESP_LOG_ERROR);
#elif CONFIG_COLLAR_LOG_PROFILE_VERBOSE
    esp_log_level_set("*", ESP_LOG_INFO);
    set_level("app_mgr", ESP_LOG_DEBUG);
    set_level("service_mgr", ESP_LOG_DEBUG);
    set_level("cloud_svc", ESP_LOG_INFO);
    set_level("conv_svc", ESP_LOG_INFO);
    set_level("supervisor", ESP_LOG_DEBUG);
    set_level("trace", ESP_LOG_DEBUG);
    set_level("httpd", ESP_LOG_ERROR);
    set_level("httpd_uri", ESP_LOG_ERROR);
    set_level("httpd_txrx", ESP_LOG_ERROR);
    set_level("httpd_parse", ESP_LOG_ERROR);
#else
    esp_log_level_set("*", ESP_LOG_WARN);
    set_level("main", ESP_LOG_INFO);
    set_level("bsp", ESP_LOG_INFO);
    set_level("hal", ESP_LOG_INFO);
    set_level("kernel", ESP_LOG_INFO);
    set_level("wifi_svc", ESP_LOG_INFO);
    set_level("cloud_svc", ESP_LOG_INFO);
    set_level("conv_svc", ESP_LOG_INFO);
    set_level("service_mgr", ESP_LOG_INFO);
    set_level("app_mgr", ESP_LOG_WARN);
    set_level("supervisor", ESP_LOG_INFO);
    set_level("trace", CONFIG_COLLAR_ENABLE_VERBOSE_BOOT_LOG ? ESP_LOG_INFO : ESP_LOG_WARN);
    set_level("httpd", ESP_LOG_ERROR);
    set_level("httpd_uri", ESP_LOG_ERROR);
    set_level("httpd_txrx", ESP_LOG_ERROR);
    set_level("httpd_parse", ESP_LOG_ERROR);
#endif
}
