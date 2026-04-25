#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t dialog_orchestrator_start(void);
void dialog_orchestrator_process_line(char *line);
bool dialog_orchestrator_is_active(void);
