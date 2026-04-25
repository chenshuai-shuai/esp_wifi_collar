#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint64_t seq;
    uint32_t sent_total;
    uint32_t dropped_total;
    uint32_t chunk_bytes;
} dialog_uplink_stats_t;

esp_err_t dialog_uplink_start(void);
void dialog_uplink_set_active(bool active);
bool dialog_uplink_is_active(void);

void dialog_uplink_reset_turn(void);
void dialog_uplink_drain_stale(void);
void dialog_uplink_get_stats(dialog_uplink_stats_t *stats);
uint32_t dialog_uplink_chunk_bytes(void);
