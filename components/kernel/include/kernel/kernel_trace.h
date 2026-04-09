#pragma once

#include <stdint.h>

void kernel_trace_boot(const char *phase);
void kernel_trace_counter(const char *name, uint32_t value);
