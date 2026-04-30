    /*
 * firmware_version.h
 *
 * Single source of truth for our hand-maintained firmware revision.
 *
 * Bump FW_VERSION_BUILD every time we ship a meaningful behavioural
 * change that we need to verify is actually on the device. Log line
 * prefixed with "FW-VER:" is printed once at boot so stale flash
 * images are immediately obvious.
 *
 * Revision history:
 *   1 - initial baseline before gRPC uplink stability fixes.
 *   4 - log_control_apply(): promote "wifi_svc" and IDF "wifi" tag
 *       to INFO so we can see the Wi-Fi state machine when
 *       conversation_client keeps reporting wifi=0.
 *   3 - log_control_apply(): whitelist "main" tag at INFO for both

 *       Normal and Verbose profiles so FW-VER + boot identity are
 *       actually printed (previously "*"=WARN silenced them).
 *   2 - gRPC uplink stability pass:

 *         * HTTP/2 SETTINGS_INITIAL_WINDOW_SIZE = 1 MiB;
 *         * tx_queue depth 128 + DROP_OLDEST policy (match Android
 *           BufferOverflow.DROP_OLDEST);
 *         * worker pump tick always resume_data() (nghttp2 cross-task);
 *         * h2_data_source_read_cb: free cur_frame when fully drained
 *           in the "new frame" path (fixes permanent DEFERRED stall
 *           that left sent_chunks stuck at 5);
 *         * CONV_START drains stale mic StreamBuffer first.
 */
#pragma once

#define FW_VERSION_BUILD      46
#define FW_VERSION_CODENAME   "nghttp2-stream-uplink"
#define FW_VERSION_STRING     "v0.0.46-nghttp2-stream-uplink"
/*
 *   46 - Replace the fragile hand-written StreamConversation DATA sender
 *        with nghttp2-managed HTTP/2 for the streaming uplink path.
 *        Keep the already-working START/STOP orchestration, raw PCM
 *        AudioChunk encoding, 5 s uplink holdoff, and minimal EndConversation
 *        h2c sender unchanged.
 */
/*
 *   45 - Align ESP uplink payload with Android app startup config:
 *        HiltApplication calls GrpcAudioClient.setEncodeAudioAsBase64(false),
 *        so AudioChunk.audio_data must carry raw PCM16 bytes directly rather
 *        than base64 ASCII.
 */
/*
 *   44 - Uplink hardening + Android wire alignment:
 *        - fix uplink stack overflows on dlg_ul / send_audio path
 *        - fix chunk fill state machine (avoid need=0 read deadlock)
 *        - align AudioChunk metadata policy with Android client
 *          (format+timestamp per chunk)
 */
/*
 *   43 - Manual uplink transport upgraded to persistent mode:
 *        single long-lived TCP+h2 connection with one StreamConversation
 *        stream kept open during session, DATA frames sent continuously,
 *        minimal SETTINGS/WINDOW_UPDATE handling and reconnect logic.
 */
/*
 *   42 - Manual uplink mode hard switch:
 *        disable nghttp2 transport/stream bring-up for CONV_START path.
 *        In manual mode, stream_writable is derived from
 *        configured+wifi+session_active, so uplink uses only hand-crafted
 *        HTTP/2 sends and no nghttp2_session_client_new allocation.
 */
/*
 *   41 - Realtime dialog modularization:
 *        split command execution into dedicated modules:
 *          dialog_connection / dialog_session / dialog_uplink /
 *          dialog_downlink / dialog_playback / dialog_orchestrator.
 *        app_manager now delegates CONV_START/CONV_STOP orchestration
 *        to the standalone orchestrator module for lower coupling.
 */
/*
 *   40 - Experimental manual uplink path:
 *        bypass nghttp2 realtime DATA sender in conversation_client_send_audio
 *        and route mic chunks to a dedicated manual uploader task that hand-
 *        crafts HTTP/2 (preface/settings/headers/data) per chunk for testing.
 */
/*
 *   39 - On transport faults, preserve active conversation intent/state
 *        so worker auto-reconnects and reopens StreamConversation instead
 *        of falling back to IDLE waiting for a new CONV_START.
 */
/*
 *   38 - Keep conversation intent across transport faults:
 *        on h2 pump/wifi faults, preserve session+state (do not force IDLE),
 *        retry transport quickly (1s), and reopen stream automatically.
 */
/*
 *   37 - Real-time backpressure policy:
 *        shorten uplink no-mem retry to a single short attempt, then drop
 *        and continue draining mic stream to avoid prolonged producer stalls
 *        and recurring microphone buffer overflow bursts.
 */
/*
 *   36 - HTTP/2 uplink stall hardening:
 *        treat nghttp2 WOULD_BLOCK as non-fatal in pump path, add periodic
 *        send probe when socket reports not writable, and force transport
 *        reconnect if tx queue stays non-empty with zero send progress.
 */
/*
 *   35 - Uplink transport efficiency pass for ESP32-C3:
 *        switch uplink packetization to 40 ms chunks, increase
 *        conversation tx queue/pool depth to 24, raise per-frame size caps,
 *        and bound backpressure retries to prevent long stalls.
 */
/*
 *   34 - Switch conversation uplink audio profile to 16kHz PCM16 mono:
 *        align both microphone capture and AudioChunk AudioFormat sample_rate
 *        to 16kHz to reduce sustained uplink bandwidth/queue pressure on C3.
 */
/*
 *   33 - Fix CPU starvation under uplink backpressure:
 *        convert sub-tick delays to guaranteed >=1 tick waits, use 20 ms
 *        real waits on mic stream buffer send/retry paths, and lower
 *        mic_capture task priority to avoid starving IDLE/TWDT.
 */
/*
 *   32 - Uplink throughput and low-power profile adaptation:
 *        h2 pump uses short select timeout + adaptive task cadence while
 *        queue is non-empty; add Wi-Fi realtime profile switch on
 *        CONV_START/CONV_STOP (MIN_MODEM + listen interval 1 during talk);
 *        microphone stream write uses short blocking send and larger
 *        stream buffer window (750ms) to reduce overflow under backpressure.
 */
/*
 *   31 - Backpressure stability fix:
 *        throttle UL pool-exhaust logs to 1Hz (avoid log storm) and add
 *        mic_capture backoff when PCM stream write is partial/full, preventing
 *        hot-spin CPU starvation and task_wdt resets under uplink congestion.
 */
/*
 *   30 - Strict no-drop-at-client uplink mode:
 *        conversation_client no longer drops oldest/newest audio on queue/pool
 *        pressure; it reports backpressure, and mic_uplink retries same chunk
 *        until accepted. Also enlarge mic PCM stream buffer window (500ms) to
 *        absorb short transport stalls on ESP32-C3.
 */
/*
 *   29 - Uplink-only adaptation:
 *        when tx pool is exhausted, reclaim slot by dropping oldest queued
 *        audio (instead of returning NO_MEM immediately) so ESP32 keeps
 *        sending freshest mic data continuously after stream open.
 *        Also shrink per-chunk buffers for C3 RAM efficiency.
 */
/*
 *   28 - Critical payload alignment fix:
 *        Android reference app sets GrpcAudioClient.setEncodeAudioAsBase64(false),
 *        but ESP fallback macro forced base64=1 when sdkconfig left option unset.
 *        Default fallback is now raw bytes (base64=0) to match Android/server.
 */
/*
 *   27 - Uplink adaptation for ESP32-C3 bandwidth/resource limits:
 *        add adaptive backpressure throttle in mic_uplink task.
 *        On queue/pool saturation, progressively increase cooldown and
 *        drain mic buffers instead of continuously enqueueing, reducing
 *        futile heap churn and stabilizing StreamConversation uplink.
 */
/*
 *   26 - ESP32-C3 memory adaptation pass:
 *        move app_console/mic_uplink tasks and conversation tx_queue to
 *        static allocation, reducing heap fragmentation and preserving
 *        larger contiguous blocks for nghttp2 session creation.
 */
/*
 *   25 - ESP32-C3 adaptation hardening:
 *        suppress idle mic-test Speex denoise whenever conversation
 *        client is configured, so it cannot fragment/consume heap
 *        needed by nghttp2 session creation on CONV_START.
 */
/*
 *   24 - ESP32-C3 memory handoff fix for CONV_START:
 *        before starting StreamConversation, explicitly deinit Speex
 *        denoise from idle mic self-test path to free large dynamic
 *        heap blocks for nghttp2_session_client_new().
 */
/*
 *   23 - Add runtime stack guard rails on ESP32-C3:
 *        track task handles for app/console/mic/end_rpc/conv_cli and
 *        periodically warn when stack high-watermark drops below
 *        safety thresholds, so stack regressions are caught before
 *        they become panics.
 */
/*
 *   22 - Fix build=21 regression: end-rpc static worker stack was too
 *        small and corrupted task/newlib state (panic in queue assert
 *        via _vfprintf_r). Raise end-rpc static stack back to 4096.
 */
/*
 *   21 - CONV_START recovery on C3 low-heap profile:
 *        add explicit conv_open_transport failure logs (including
 *        nghttp2 stage + free_heap/largest block), and move end-rpc
 *        queue/worker to static allocation so STOP async path no
 *        longer consumes fragmented heap needed by nghttp2 session.
 */
/*
 *   20 - end-rpc minimal sender reliability guard:
 *        treat persistent EAGAIN/EWOULDBLOCK as hard failure and
 *        reject short-send as FAIL. Prevents false "OK" when not all
 *        bytes were actually flushed to the socket.
 */
/*
 *   19 - Throw away nghttp2 for EndConversation entirely. Build a
 *        raw HTTP/2 PRI preface + SETTINGS + HEADERS + DATA(END_STREAM)
 *        on a 1200-byte stack buffer, one send(), done. Uses 0 B of
 *        heap so it cannot OOM regardless of Wi-Fi / speex pressure.
 *        This is what finally makes CONV_STOP -> server work on
 *        ESP32-C3 where largest free block is ~4 KB after boot.
 */

/*
 *   18 - Async CONV_STOP dispatch: app_manager adds a dedicated
 *        "end_rpc" worker task + session-id queue so the UART reader
 *        returns immediately after parsing. A second CONV_START or
 *        CONV_STOP is no longer blocked on the previous unary
 *        EndConversation round-trip. Combined with build=17's
 *        lazy-open reuse path, CONV_START / CONV_STOP now behave as
 *        fully independent fire-and-forget commands.
 */

/*
 *   17 - Extend build=16 reuse-channel fix to cover the "CONV_STOP
 *        without prior CONV_START" case: when the main channel is
 *        not yet up at send_end_rpc() entry, call conv_open_transport()
 *        first so we can still multiplex on it instead of falling
 *        back to the independent-session path that reliably OOMs on
 *        ESP32-C3 (build=16 log: largest_free_block=7168 B, needed ~8 KB).
 */

/*
 *   16 - Fix: send_end_rpc() multiplexes EndConversation on the
 *        long-lived s_conv.h2 instead of spawning a second
 *        nghttp2_session_client_new (which fails with OOM on
 *        ESP32-C3 after Wi-Fi + speex consume most heap; see build=15
 *        log "nghttp2_session_client_new failed: Out of memory").
 *        Main h2 callbacks now route end-rpc stream_id to the
 *        dedicated end_rpc_* handlers; conv_task's pump drives the
 *        RPC through. Falls back to independent TCP only if main
 *        channel isn't up.
 */
