#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "kernel/kernel_msgbus.h"

esp_err_t conversation_service_start(void);
void conversation_service_handle_wifi_state(kernel_wifi_state_t state);
void conversation_service_log_status(void);
bool conversation_service_is_configured(void);
const char *conversation_service_host(void);
uint16_t conversation_service_port(void);
