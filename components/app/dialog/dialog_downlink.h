#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

esp_err_t dialog_downlink_start(void);
void dialog_downlink_reset_turn(void);

#if CONFIG_COLLAR_QEMU_OPENETH
/*
 * QEMU-only: register a callback fired the first time we see audio_output
 * (or audio_start) within one server turn. Used by the qemu_user_loop
 * state machine to switch the uplink mic-mode immediately when the
 * server begins replying. Pass NULL to detach.
 *
 * The callback runs on the conversation worker task; keep it short and
 * non-blocking.
 */
typedef void (*qemu_first_audio_cb_t)(void);
void dialog_downlink_qemu_set_first_audio_cb(qemu_first_audio_cb_t cb);
#endif
