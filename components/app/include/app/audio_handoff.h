#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_handoff_init(void);
esp_err_t audio_handoff_uart_ready(void);

/* Returns true when the line is an nRF audio-handoff command. */
bool audio_handoff_process_line(char *line);

esp_err_t audio_handoff_enter_safe(void);
esp_err_t audio_handoff_enter_esp_owner(void);

bool audio_handoff_is_esp_owner(void);
const char *audio_handoff_state_string(void);

#ifdef __cplusplus
}
#endif
