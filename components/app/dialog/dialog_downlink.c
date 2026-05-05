#include "dialog_downlink.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "conversation/conversation_client.h"

#include "dialog_playback.h"
#include "dialog_uplink.h"

static const char *TAG = "dlg_dl";

static bool s_started;

#if CONFIG_COLLAR_QEMU_OPENETH
/*
 * QEMU long-run "10 rounds in 1 stream" turn-taking model
 * -------------------------------------------------------
 * In the QEMU build the speaker is not wired (no real DAC), and we want
 * a virtual-user state machine in qemu_user_loop.c to be able to flip
 * uplink mode the moment the server BEGINS replying (first audio_output
 * bytes), not when the reply is fully delivered (audio_complete). To do
 * that we expose a single "first audio_output of the current turn"
 * callback that the user-loop registers, and we DROP the audio_output
 * payload entirely (no decode, no playback queue, no memory pressure).
 *
 * This is intentionally only enabled in the QEMU build. On real
 * hardware the original "write to dialog_playback" path is used.
 */
typedef void (*qemu_first_audio_cb_t)(void);
static qemu_first_audio_cb_t s_qemu_first_audio_cb;
static bool                  s_qemu_seen_audio_output_in_turn;

void dialog_downlink_qemu_set_first_audio_cb(qemu_first_audio_cb_t cb)
{
    s_qemu_first_audio_cb = cb;
}
#endif /* CONFIG_COLLAR_QEMU_OPENETH */

static void dialog_downlink_on_audio_output(const uint8_t *pcm,
                                            size_t len,
                                            int64_t seq,
                                            void *arg)
{
    (void)arg;
#if CONFIG_COLLAR_QEMU_OPENETH
    /* v17 — make the routing visible.
     *
     * Conversation_client decodes ConversationEvent.audio_output via
     * pb_decode_bytes_discard(), which (a) walks every PCM byte without
     * copying and (b) leaves pcm/len here as NULL/0 in the QEMU build.
     * That is INTENTIONAL: real hardware uses pb_decode_string/copy
     * into a playback buffer, but in QEMU we have no DAC and no need
     * to keep the bytes around — we only care that they came in and
     * how many.
     *
     * What we DO care about (and what the user asked for):
     *   - did we get the listener callback at all?
     *   - what's the sequence_number / cumulative count for THIS turn?
     *   - did the very first audio_output of the turn fire the
     *     "server reply began" hook (so qemu_user_loop can switch
     *     uplink off in a real-hardware-like way)?
     *
     * So instead of `(void)pcm; (void)len;` we now log every call. */
    static uint32_t s_qemu_audio_in_turn;
    static uint32_t s_qemu_audio_total;
    s_qemu_audio_in_turn++;
    s_qemu_audio_total++;
    ESP_LOGI(TAG,
             "QEMU audio_output: turn_evt#%lu total#%lu seq=%lld pcm_ptr=%p pcm_len=%u (discarded)",
             (unsigned long)s_qemu_audio_in_turn,
             (unsigned long)s_qemu_audio_total,
             (long long)seq,
             (const void *)pcm,
             (unsigned)len);
    if (!s_qemu_seen_audio_output_in_turn) {
        s_qemu_seen_audio_output_in_turn = true;
        ESP_LOGI(TAG, "first audio_output in turn (server reply began)");
        if (s_qemu_first_audio_cb != NULL) {
            s_qemu_first_audio_cb();
        }
    }
    /* arm next-turn counter from on_audio_complete */
    (void)s_qemu_audio_in_turn;
#else
    (void)seq;
    (void)dialog_playback_write(pcm, len);
#endif
}


static void dialog_downlink_on_audio_complete(void *arg)
{
    (void)arg;
#if CONFIG_COLLAR_QEMU_OPENETH
    /* End of one server turn: arm the "first audio_output" detector for
     * the next turn. We deliberately do NOT call dialog_uplink_resume_now
     * here; the qemu_user_loop drives uplink directly via mic-mode. */
    s_qemu_seen_audio_output_in_turn = false;
    ESP_LOGI(TAG, "audio_complete (qemu): downlink dropped, awaiting next-turn audio_output");
#else
    if (conversation_client_session_active()) {
        dialog_uplink_resume_now();
        ESP_LOGI(TAG, "audio_complete: discarded downlink, uplink resumed");
    } else {
        ESP_LOGI(TAG, "audio_complete: discarded downlink, session inactive");
    }
#endif
}

static void dialog_downlink_on_audio_start(void *arg)
{
    (void)arg;
#if CONFIG_COLLAR_QEMU_OPENETH
    /* Server says "I'm about to speak". Same semantic as first
     * audio_output for our purposes; route through the same hook so
     * the user-loop only has one trigger to listen for. */
    if (!s_qemu_seen_audio_output_in_turn) {
        s_qemu_seen_audio_output_in_turn = true;
        ESP_LOGI(TAG, "audio_start (qemu): server began TTS");
        if (s_qemu_first_audio_cb != NULL) {
            s_qemu_first_audio_cb();
        }
    }
#else
    if (dialog_uplink_is_active()) {
        dialog_uplink_set_active(false);
        ESP_LOGI(TAG, "audio_start: uplink stopped, downlink discard mode");
    } else {
        ESP_LOGI(TAG, "audio_start: downlink discard mode");
    }
#endif
}

static void dialog_downlink_on_session_start(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "session_start");
}

static void dialog_downlink_on_error(const char *code, const char *message, void *arg)
{
    (void)arg;
    ESP_LOGW(TAG,
             "server ErrorEvent code='%s' msg='%s'",
             code ? code : "-",
             message ? message : "-");
}

esp_err_t dialog_downlink_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    conversation_client_set_audio_output_listener(dialog_downlink_on_audio_output, NULL);
    conversation_client_set_audio_complete_listener(dialog_downlink_on_audio_complete, NULL);
    conversation_client_set_audio_start_listener(dialog_downlink_on_audio_start, NULL);
    conversation_client_set_session_start_listener(dialog_downlink_on_session_start, NULL);
    conversation_client_set_error_listener(dialog_downlink_on_error, NULL);

    s_started = true;
    return ESP_OK;
}

void dialog_downlink_reset_turn(void)
{
    dialog_playback_reset_counters();
}
