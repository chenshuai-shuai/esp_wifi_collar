#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kernel/kernel_msgbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * conversation_client
 *
 *   gRPC client for traini.ConversationService on ESP32-C3.
 *   Transport: HTTP/2 cleartext (h2c) over lwIP sockets. StreamConversation
 *              uses nghttp2 for SETTINGS/HPACK/DATA/flow control; the already
 *              stable EndConversation path remains a minimal short-lived h2c
 *              sender.
 *   Payload:   gRPC length-prefixed Protobuf (nanopb). AudioChunk.audio_data
 *              carries raw PCM16 bytes by default, matching the Android app's
 *              GrpcAudioClient.setEncodeAudioAsBase64(false) startup setting.
 *
 *   This header exposes both:
 *     - low-level lifecycle helpers (start/send_audio/end_session) used by
 *       the initial self-test and by the kernel (Wi-Fi state plumbing);
 *     - a high-level conversation state machine that mirrors the Android
 *       ConversationViewModel, so app logic can drive a full turn-taking
 *       dialog (start_conversation -> start_talking -> stop_talking ->
 *       wait for server reply -> end_conversation).
 */

typedef enum {
    CONVERSATION_STATE_IDLE = 0,
    CONVERSATION_STATE_CONNECTING,
    CONVERSATION_STATE_READY,
    CONVERSATION_STATE_TALKING,
    CONVERSATION_STATE_WAITING_RESPONSE,
} conversation_state_t;

/* Listener signatures. All invoked from the conversation worker task
 * (not from IRQ). Implementations should be short / non-blocking. */
typedef void (*conversation_audio_output_cb_t)(const uint8_t *pcm,
                                               size_t len,
                                               int64_t sequence_number,
                                               void *arg);
typedef void (*conversation_event_cb_t)(void *arg);
typedef void (*conversation_error_cb_t)(const char *code,
                                        const char *message,
                                        void *arg);

/* ------------- lifecycle / low level ------------------------------- */

esp_err_t conversation_client_start(void);
void      conversation_client_handle_wifi_state(kernel_wifi_state_t state);
void      conversation_client_log_status(void);

bool        conversation_client_is_configured(void);
const char *conversation_client_host(void);
uint16_t    conversation_client_port(void);

bool conversation_client_transport_ready(void);
bool conversation_client_stream_ready(void);
bool conversation_client_stream_writable(void);
bool conversation_client_session_active(void);
bool conversation_client_uplink_can_accept(void);

esp_err_t conversation_client_start_session(const char *session_id);
esp_err_t conversation_client_end_session(void);
esp_err_t conversation_client_send_audio(const uint8_t *pcm,
                                         size_t len,
                                         uint64_t seq);

/* ------------- listeners ------------------------------------------ */

void conversation_client_set_audio_output_listener(conversation_audio_output_cb_t cb, void *arg);
void conversation_client_set_audio_complete_listener(conversation_event_cb_t cb, void *arg);
void conversation_client_set_audio_start_listener(conversation_event_cb_t cb, void *arg);
void conversation_client_set_session_start_listener(conversation_event_cb_t cb, void *arg);
void conversation_client_set_error_listener(conversation_error_cb_t cb, void *arg);

/* ------------- high-level state machine --------------------------- */

conversation_state_t conversation_client_get_state(void);
const char          *conversation_client_state_str(conversation_state_t s);

/* Begin a logical conversation. Internally makes sure a StreamConversation
 * RPC is open on the current (or a freshly-arming) h2c transport and moves
 * the state to CONNECTING; once the server delivers its first event the
 * session_start listener is invoked and the state advances to READY. */
esp_err_t conversation_client_start_conversation(const char *session_id);

/* READY -> TALKING. Uplink audio is expected to be pushed by the caller
 * via conversation_client_send_audio() after this call. */
esp_err_t conversation_client_start_talking(void);

/* TALKING -> WAITING_RESPONSE. Uplink is paused until the downlink
 * audio_complete event moves the state back to READY. */
esp_err_t conversation_client_stop_talking(void);

/* Terminate the conversation. Half-closes the streaming RPC, issues a
 * unary EndConversation RPC on the same h2 session, and tears the
 * transport down. Moves the state to IDLE. */
esp_err_t conversation_client_end_conversation(void);

/* Fire-and-wait unary EndConversation RPC with an arbitrary session_id.
 * Opens a brand-new short-lived h2c connection to the same gRPC server,
 * sends a single EndConversationRequest{session_id}, waits up to 3 s for
 * the SessionSummary reply, tears the connection down. Safe to call at
 * any time from any task: it is independent of the streaming RPC state
 * machine and does not interfere with an active StreamConversation. */
esp_err_t conversation_client_send_end_rpc(const char *session_id);

/* Abort current active session immediately.
 * Behavior:
 *  1) log explicit reason;
 *  2) tear down current streaming session and move to IDLE;
 *  3) fire one-shot EndConversation RPC for the current session_id.
 * Returns ESP_OK when abort request is accepted. */
esp_err_t conversation_client_abort_session(const char *reason, esp_err_t cause);

/* Read-only current session id (may be empty string). */
const char *conversation_client_current_session_id(void);


#ifdef __cplusplus
}
#endif
