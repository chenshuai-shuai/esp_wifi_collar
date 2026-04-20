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
bool conversation_service_transport_ready(void);
bool conversation_service_stream_ready(void);
bool conversation_service_stream_writable(void);
bool conversation_service_session_active(void);
esp_err_t conversation_service_start_session(const char *session_id);
esp_err_t conversation_service_end_session(void);
esp_err_t conversation_service_send_audio(const uint8_t *pcm, size_t len, uint64_t seq);
