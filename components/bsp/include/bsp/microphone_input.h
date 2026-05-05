#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

esp_err_t bsp_microphone_init(void);
void bsp_microphone_deinit(void);
bool bsp_microphone_is_ready(void);
uint32_t bsp_microphone_get_sample_rate_hz(void);
uint8_t bsp_microphone_get_channels(void);
size_t bsp_microphone_frame_bytes(void);
esp_err_t bsp_microphone_read(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);

#if CONFIG_COLLAR_QEMU_OPENETH
/*
 * QEMU virtual microphone test-pattern selector.
 *
 * The QEMU build replaces the real PDM driver with a generator. By default
 * (BSP_QEMU_MIC_MODE_SILENCE) it emits low-amplitude (±64) white noise -
 * a "background hiss" that keeps the gRPC DATA stream alive without
 * tripping server-side VAD as voice.
 *
 * BSP_QEMU_MIC_MODE_VOICE_ONESHOT plays the bundled real-voice PCM
 * fixture (test_voice_pcm.h, ~3.7s English phrase) exactly once from the
 * beginning and then automatically reverts to SILENCE. The state machine
 * in components/app/qemu_user_loop.c uses this to send "silence prelude
 * -> one utterance -> silence tail -> wait for server -> repeat" turn-
 * taking, mirroring the real-user behaviour for an Android client. We
 * never loop the fixture; "user" is supposed to say the same line once
 * per turn, not on a repeat.
 */
typedef enum {
    BSP_QEMU_MIC_MODE_SILENCE = 0,
    BSP_QEMU_MIC_MODE_VOICE_ONESHOT,
} bsp_qemu_mic_mode_t;

void bsp_qemu_mic_set_mode(bsp_qemu_mic_mode_t mode);
bsp_qemu_mic_mode_t bsp_qemu_mic_get_mode(void);

/* True iff the most recent VOICE_ONESHOT request has finished playing
 * the fixture and the generator has reverted to SILENCE. */
bool bsp_qemu_mic_voice_done(void);
#endif /* CONFIG_COLLAR_QEMU_OPENETH */
