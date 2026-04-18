#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "kernel/kernel_msgbus.h"

esp_err_t cloud_service_start(void);
void cloud_service_handle_wifi_state(kernel_wifi_state_t state);
void cloud_service_log_status(void);
bool cloud_service_is_reachable(void);
bool cloud_service_has_attempted_probe(void);
const char *cloud_service_host(void);
const char *cloud_service_last_error(void);
uint16_t cloud_service_port(void);
