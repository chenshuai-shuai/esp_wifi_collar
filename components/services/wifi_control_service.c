#include "services/wifi_control_service.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "services/wifi_service.h"

#define WIFI_UART_CMD_STACK_WORDS       3072
#define WIFI_UART_CMD_PRIORITY          7
#define WIFI_UART_CMD_MAX_LEN           128

#define WIFI_UART_CMD_REPROVISION       "ESP:WIFI_REPROVISION"
#define WIFI_UART_RSP_REPROVISION_OK    "OK:WIFI_REPROVISION"

static const char *TAG = "wifi_ctrl";

static StaticTask_t s_wifi_ctrl_tcb;
static StackType_t s_wifi_ctrl_stack[WIFI_UART_CMD_STACK_WORDS];
static bool s_wifi_ctrl_started;

static void wifi_control_send_response(const char *response)
{
    printf("%s\n", response);
    fflush(stdout);
}

static void wifi_control_send_error(const char *command, esp_err_t err)
{
    char response[96];
    snprintf(response, sizeof(response), "ERR:%s:%s", command, esp_err_to_name(err));
    wifi_control_send_response(response);
}

static void wifi_control_handle_command(const char *line)
{
    if (strcmp(line, WIFI_UART_CMD_REPROVISION) == 0) {
        ESP_LOGI(TAG, "UART control command received: %s", line);
        esp_err_t ret = wifi_service_request_reprovision();
        if (ret == ESP_OK) {
            wifi_control_send_response(WIFI_UART_RSP_REPROVISION_OK);
        } else {
            wifi_control_send_error("WIFI_REPROVISION", ret);
        }
        return;
    }

    ESP_LOGW(TAG, "Unknown UART control command: %s", line);
}

static void wifi_control_task(void *arg)
{
    (void)arg;

    char line[WIFI_UART_CMD_MAX_LEN];
    size_t len = 0;

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (len > 0U) {
                line[len] = '\0';
                wifi_control_handle_command(line);
                len = 0U;
            }
            continue;
        }

        if (!isprint((unsigned char)ch)) {
            continue;
        }

        if (len < (sizeof(line) - 1U)) {
            line[len++] = (char)ch;
        } else {
            len = 0U;
            ESP_LOGW(TAG, "UART control command too long, dropping line");
        }
    }
}

esp_err_t wifi_control_service_start(void)
{
    if (s_wifi_ctrl_started) {
        return ESP_OK;
    }

    TaskHandle_t task_handle = xTaskCreateStatic(
        wifi_control_task,
        "wifi_ctrl",
        WIFI_UART_CMD_STACK_WORDS,
        NULL,
        WIFI_UART_CMD_PRIORITY,
        s_wifi_ctrl_stack,
        &s_wifi_ctrl_tcb);
    if (task_handle == NULL) {
        return ESP_FAIL;
    }

    s_wifi_ctrl_started = true;
    ESP_LOGI(TAG, "Wi-Fi control UART service started");
    return ESP_OK;
}
