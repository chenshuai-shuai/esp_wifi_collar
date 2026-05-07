#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t dialog_orchestrator_start(void);
void dialog_orchestrator_set_auto_enabled(bool enabled, const char *reason);
void dialog_orchestrator_auto_tick(int64_t now_us);
void dialog_orchestrator_abort_local(const char *reason);
bool dialog_orchestrator_is_active(void);
