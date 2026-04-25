#include "services/cloud_service.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#ifdef CONFIG_COLLAR_CLOUD_PROBE_ENABLE
#define CLOUD_PROBE_ENABLED 1
#else
#define CLOUD_PROBE_ENABLED 0
#endif


#define CLOUD_SERVICE_STACK_WORDS      3072
#define CLOUD_SERVICE_PRIORITY         8
#define CLOUD_SERVICE_HOST_MAX_LEN     128
#define CLOUD_SERVICE_ADDR_STR_LEN     64
#define CLOUD_SERVICE_ERROR_STR_LEN    96

static const char *TAG = "cloud_svc";

typedef struct {
    bool started;
    bool wifi_ready;
    bool configured;
    bool reachable;
    bool dns_resolved;
    uint16_t port;
    uint32_t success_count;
    uint32_t failure_count;
    int64_t last_probe_us;
    char host[CLOUD_SERVICE_HOST_MAX_LEN];
    char resolved_addr[CLOUD_SERVICE_ADDR_STR_LEN];
    char last_error[CLOUD_SERVICE_ERROR_STR_LEN];
} cloud_service_state_t;

static cloud_service_state_t s_cloud;
static StaticTask_t s_cloud_tcb;
static StackType_t s_cloud_stack[CLOUD_SERVICE_STACK_WORDS];

static void cloud_service_set_error(const char *error)
{
    strlcpy(s_cloud.last_error, error != NULL ? error : "", sizeof(s_cloud.last_error));
}

static void cloud_service_format_sockaddr(const struct sockaddr *addr, char *out, size_t out_len)
{
    if (out_len == 0U) {
        return;
    }

    out[0] = '\0';

    if (addr == NULL) {
        return;
    }

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &ipv4->sin_addr, out, out_len);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &ipv6->sin6_addr, out, out_len);
    } else {
        strlcpy(out, "unknown_af", out_len);
    }
}

static esp_err_t cloud_service_connect_with_timeout(int sock, const struct sockaddr *addr,
                                                    socklen_t addr_len, int timeout_ms)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return ESP_FAIL;
    }

    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        return ESP_FAIL;
    }

    int ret = connect(sock, addr, addr_len);
    if (ret == 0) {
        (void)fcntl(sock, F_SETFL, flags);
        return ESP_OK;
    }

    if (errno != EINPROGRESS) {
        (void)fcntl(sock, F_SETFL, flags);
        return ESP_FAIL;
    }

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);

    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    ret = select(sock + 1, NULL, &writefds, NULL, &timeout);
    if (ret <= 0) {
        (void)fcntl(sock, F_SETFL, flags);
        errno = (ret == 0) ? ETIMEDOUT : errno;
        return ESP_ERR_TIMEOUT;
    }

    int so_error = 0;
    socklen_t opt_len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &opt_len) != 0) {
        (void)fcntl(sock, F_SETFL, flags);
        return ESP_FAIL;
    }

    (void)fcntl(sock, F_SETFL, flags);
    if (so_error != 0) {
        errno = so_error;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cloud_service_probe_once(void)
{
    char port_str[8];
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    struct addrinfo *entry = NULL;
    int gai_ret;
    esp_err_t ret = ESP_FAIL;

    s_cloud.last_probe_us = esp_timer_get_time();
    s_cloud.dns_resolved = false;
    s_cloud.resolved_addr[0] = '\0';
    cloud_service_set_error("");

    snprintf(port_str, sizeof(port_str), "%u", (unsigned int)s_cloud.port);
    gai_ret = getaddrinfo(s_cloud.host, port_str, &hints, &result);
    if (gai_ret != 0 || result == NULL) {
        s_cloud.failure_count++;
        snprintf(s_cloud.last_error, sizeof(s_cloud.last_error), "getaddrinfo=%d", gai_ret);
        return ESP_FAIL;
    }

    for (entry = result; entry != NULL; entry = entry->ai_next) {
        int sock = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (sock < 0) {
            continue;
        }

        cloud_service_format_sockaddr(entry->ai_addr, s_cloud.resolved_addr, sizeof(s_cloud.resolved_addr));
        s_cloud.dns_resolved = true;
        ret = cloud_service_connect_with_timeout(
            sock, entry->ai_addr, (socklen_t)entry->ai_addrlen, CONFIG_COLLAR_CLOUD_CONNECT_TIMEOUT_MS);
        close(sock);

        if (ret == ESP_OK) {
            s_cloud.reachable = true;
            s_cloud.success_count++;
            freeaddrinfo(result);
            cloud_service_set_error("");
            return ESP_OK;
        }
    }

    s_cloud.failure_count++;
    s_cloud.reachable = false;
    if (errno != 0) {
        snprintf(s_cloud.last_error, sizeof(s_cloud.last_error), "connect errno=%d", errno);
    } else {
        cloud_service_set_error("connect failed");
    }
    freeaddrinfo(result);
    return ESP_FAIL;
}

static bool cloud_service_probe_due(int64_t now_us)
{
    const int64_t interval_us = (int64_t)CONFIG_COLLAR_CLOUD_PROBE_INTERVAL_SEC * 1000000LL;

    if (s_cloud.last_probe_us == 0) {
        return true;
    }

    return (now_us - s_cloud.last_probe_us) >= interval_us;
}

static void cloud_service_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (!CLOUD_PROBE_ENABLED) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!s_cloud.configured) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!s_cloud.wifi_ready) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (cloud_service_probe_due(esp_timer_get_time())) {
            esp_err_t ret = cloud_service_probe_once();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Cloud probe ok: host=%s port=%u addr=%s",
                         s_cloud.host, (unsigned int)s_cloud.port,
                         s_cloud.resolved_addr[0] != '\0' ? s_cloud.resolved_addr : "-");
            } else {
                ESP_LOGW(TAG, "Cloud probe failed: host=%s port=%u error=%s",
                         s_cloud.host, (unsigned int)s_cloud.port,
                         s_cloud.last_error[0] != '\0' ? s_cloud.last_error : "unknown");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t cloud_service_start(void)
{
    if (s_cloud.started) {
        return ESP_OK;
    }

    memset(&s_cloud, 0, sizeof(s_cloud));
    strlcpy(s_cloud.host, CONFIG_COLLAR_CLOUD_HOST, sizeof(s_cloud.host));
    s_cloud.port = (uint16_t)CONFIG_COLLAR_CLOUD_PORT;

    s_cloud.configured = CLOUD_PROBE_ENABLED && s_cloud.host[0] != '\0';

    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        cloud_service_task,
        "cloud_svc",
        CLOUD_SERVICE_STACK_WORDS,
        NULL,
        CLOUD_SERVICE_PRIORITY,
        s_cloud_stack,
        &s_cloud_tcb,
        tskNO_AFFINITY);
    if (task_handle == NULL) {
        return ESP_FAIL;
    }

    s_cloud.started = true;

    if (!CLOUD_PROBE_ENABLED) {
        ESP_LOGI(TAG, "Cloud probe disabled by config");
    } else if (!s_cloud.configured) {
        ESP_LOGI(TAG, "Cloud probe idle: host not configured");
    } else {
        ESP_LOGI(TAG, "Cloud probe armed: host=%s port=%u interval=%d s timeout=%d ms",
                 s_cloud.host, (unsigned int)s_cloud.port,
                 CONFIG_COLLAR_CLOUD_PROBE_INTERVAL_SEC,
                 CONFIG_COLLAR_CLOUD_CONNECT_TIMEOUT_MS);
    }

    return ESP_OK;
}

void cloud_service_handle_wifi_state(kernel_wifi_state_t state)
{
    switch (state) {
    case KERNEL_WIFI_STATE_GOT_IP:
        s_cloud.wifi_ready = true;
        break;

    case KERNEL_WIFI_STATE_PROVISIONING:
    case KERNEL_WIFI_STATE_CONNECTING:
    case KERNEL_WIFI_STATE_DISCONNECTED:
    case KERNEL_WIFI_STATE_FAILED:
        s_cloud.wifi_ready = false;
        s_cloud.reachable = false;
        s_cloud.dns_resolved = false;
        s_cloud.resolved_addr[0] = '\0';
        break;

    default:
        break;
    }
}

void cloud_service_log_status(void)
{
    const char *state;

    if (!CLOUD_PROBE_ENABLED) {
        state = "disabled";
    } else if (!s_cloud.configured) {
        state = "unconfigured";
    } else if (!s_cloud.wifi_ready) {
        state = "wait_wifi";
    } else if (s_cloud.reachable) {
        state = "tcp_ready";
    } else if (s_cloud.last_probe_us != 0) {
        state = "tcp_failed";
    } else {
        state = "wait_tcp_probe";
    }

    ESP_LOGI(TAG,
             "Cloud status: state=%s host=%s port=%u addr=%s wifi=%s ok=%lu fail=%lu err=%s",
             state,
             s_cloud.host[0] != '\0' ? s_cloud.host : "-",
             (unsigned int)s_cloud.port,
             s_cloud.resolved_addr[0] != '\0' ? s_cloud.resolved_addr : "-",
             s_cloud.wifi_ready ? "ready" : "no",
             (unsigned long)s_cloud.success_count,
             (unsigned long)s_cloud.failure_count,
             s_cloud.last_error[0] != '\0' ? s_cloud.last_error : "-");
}

bool cloud_service_is_reachable(void)
{
    return s_cloud.reachable;
}

bool cloud_service_has_attempted_probe(void)
{
    return s_cloud.last_probe_us != 0;
}

const char *cloud_service_host(void)
{
    return s_cloud.host;
}

const char *cloud_service_last_error(void)
{
    return s_cloud.last_error;
}

uint16_t cloud_service_port(void)
{
    return s_cloud.port;
}
