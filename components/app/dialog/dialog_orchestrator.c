#include "app/dialog_orchestrator.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "dialog_connection.h"
#include "dialog_downlink.h"
#include "dialog_playback.h"
#include "dialog_session.h"
#include "dialog_uplink.h"

#define DIALOG_END_RPC_QUEUE_DEPTH   4
#define DIALOG_END_RPC_SID_MAX       72
#define DIALOG_END_RPC_STACK_WORDS   4096

typedef struct {
    char session_id[DIALOG_END_RPC_SID_MAX];
} dialog_end_rpc_req_t;

static const char *TAG = "dlg_orch";

static bool s_started;
static bool s_active;

static QueueHandle_t s_end_rpc_queue;
static StaticQueue_t s_end_rpc_queue_struct;
static uint8_t s_end_rpc_queue_storage[
    DIALOG_END_RPC_QUEUE_DEPTH * sizeof(dialog_end_rpc_req_t)
];
static StaticTask_t s_end_rpc_tcb;
static StackType_t s_end_rpc_stack[DIALOG_END_RPC_STACK_WORDS];

static void dialog_end_rpc_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "end-rpc worker ready");

    for (;;) {
        dialog_end_rpc_req_t req;
        if (xQueueReceive(s_end_rpc_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "end-rpc dispatch sid='%s'", req.session_id);
        esp_err_t rv = dialog_session_send_end_rpc(req.session_id);
        ESP_LOGI(TAG, "end-rpc result: %s", rv == ESP_OK ? "OK" : "FAIL");
    }
}

static void dialog_handle_start(void)
{
    const conversation_state_t st = dialog_session_state();
    if (st != CONVERSATION_STATE_IDLE) {
        ESP_LOGW(TAG,
                 "CONV_START ignored, current state=%s",
                 dialog_connection_state_str(st));
        s_active = true;
        dialog_uplink_set_active(true);
        return;
    }

    ESP_LOGI(TAG, ">>> CONV_START");
    (void)dialog_connection_prepare_for_start();

    dialog_uplink_reset_turn();
    dialog_downlink_reset_turn();
    dialog_uplink_drain_stale();

    esp_err_t ret = dialog_session_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "start failed: 0x%x", ret);
        dialog_connection_restore_idle_profile();
        s_active = false;
        dialog_uplink_set_active(false);
        return;
    }

    s_active = true;
    dialog_uplink_set_active(true);
}

static void dialog_handle_stop(const char *session_id)
{
    if (session_id == NULL || session_id[0] == '\0') {
        ESP_LOGW(TAG, "CONV_STOP requires <session_id>");
        return;
    }

    dialog_uplink_stats_t ul = {0};
    dialog_uplink_get_stats(&ul);

    ESP_LOGI(TAG,
             ">>> CONV_STOP sid='%s' (uplink sent=%u dropped=%u played=%u)",
             session_id,
             (unsigned)ul.sent_total,
             (unsigned)ul.dropped_total,
             (unsigned)dialog_playback_frames_played());

    s_active = false;
    dialog_uplink_set_active(false);
    dialog_connection_restore_idle_profile();

    (void)dialog_session_stop_local();

    if (s_end_rpc_queue == NULL) {
        esp_err_t rv = dialog_session_send_end_rpc(session_id);
        ESP_LOGI(TAG, "end-rpc sync fallback: %s", rv == ESP_OK ? "OK" : "FAIL");
        return;
    }

    dialog_end_rpc_req_t req = {0};
    strlcpy(req.session_id, session_id, sizeof(req.session_id));
    if (xQueueSend(s_end_rpc_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "end-rpc queue full, drop sid='%s'", session_id);
        return;
    }

    ESP_LOGI(TAG, "CONV_STOP dispatched async");
}

esp_err_t dialog_orchestrator_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(dialog_downlink_start());

    esp_err_t ret = dialog_uplink_start();
    if (ret != ESP_OK) {
        return ret;
    }

    s_end_rpc_queue = xQueueCreateStatic(
        DIALOG_END_RPC_QUEUE_DEPTH,
        sizeof(dialog_end_rpc_req_t),
        s_end_rpc_queue_storage,
        &s_end_rpc_queue_struct);
    if (s_end_rpc_queue == NULL) {
        ESP_LOGW(TAG, "end-rpc queue create failed (sync fallback)");
    } else {
        TaskHandle_t worker = xTaskCreateStatic(
            dialog_end_rpc_worker,
            "dlg_end_rpc",
            DIALOG_END_RPC_STACK_WORDS,
            NULL,
            3,
            s_end_rpc_stack,
            &s_end_rpc_tcb);
        if (worker == NULL) {
            ESP_LOGW(TAG, "end-rpc worker create failed (sync fallback)");
            vQueueDelete(s_end_rpc_queue);
            s_end_rpc_queue = NULL;
        }
    }

    ESP_LOGI(TAG,
             "orchestrator ready (uplink chunk=%u B)",
             (unsigned)dialog_uplink_chunk_bytes());

    s_started = true;
    return ESP_OK;
}

void dialog_orchestrator_process_line(char *line)
{
    if (line == NULL) {
        return;
    }

    while (*line == ' ' || *line == '\t') {
        line++;
    }

    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' ||
                     line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    if (n == 0) {
        return;
    }

    ESP_LOGI(TAG, "cmd='%s'", line);

    if (strcmp(line, "ESP:CONV_START") == 0) {
        dialog_handle_start();
        return;
    }

    const char *stop_prefix = "ESP:CONV_STOP:";
    size_t stop_len = strlen(stop_prefix);
    if (strncmp(line, stop_prefix, stop_len) == 0) {
        dialog_handle_stop(line + stop_len);
        return;
    }

    ESP_LOGW(TAG, "unknown command (expected ESP:CONV_START | ESP:CONV_STOP:<sid>)");
}

bool dialog_orchestrator_is_active(void)
{
    return s_active;
}
