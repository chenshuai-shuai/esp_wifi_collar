#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t wifi_service_start(void);
esp_err_t wifi_service_request_reprovision(void);
void wifi_service_log_status(void);
bool wifi_service_sta_ready(void);
const char *wifi_service_sta_ip(void);
size_t wifi_service_sta_ip_copy(char *out, size_t out_len);
