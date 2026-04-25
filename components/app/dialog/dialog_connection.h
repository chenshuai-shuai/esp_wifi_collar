#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "conversation/conversation_client.h"

esp_err_t dialog_connection_prepare_for_start(void);
void dialog_connection_restore_idle_profile(void);

conversation_state_t dialog_connection_state(void);
const char *dialog_connection_state_str(conversation_state_t state);
bool dialog_connection_stream_writable(void);
