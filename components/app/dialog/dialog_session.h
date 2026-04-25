#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "conversation/conversation_client.h"

esp_err_t dialog_session_start(void);
esp_err_t dialog_session_stop_local(void);
esp_err_t dialog_session_send_end_rpc(const char *session_id);

bool dialog_session_is_idle(void);
conversation_state_t dialog_session_state(void);
