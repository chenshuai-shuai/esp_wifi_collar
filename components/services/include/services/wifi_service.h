#pragma once

#include "esp_err.h"

esp_err_t wifi_service_start(void);
esp_err_t wifi_service_request_reprovision(void);
void wifi_service_log_status(void);
