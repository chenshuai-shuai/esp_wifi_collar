/*
 * Adapted from the ESP-IDF captive portal example.
 */

#include "services/dns_server.h"

#include <errno.h>
#include <string.h>
#include <sys/param.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#define DNS_PORT                53
#define DNS_REPLY_BUFFER_LEN    256
#define DNS_A_RECORD            0x0001
#define DNS_OPCODE_MASK         0x7800
#define DNS_QR_FLAG             (1 << 7)
#define DNS_ANSWER_TTL_SEC      300

static const char *TAG = "dns_svc";

typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

struct dns_server_handle {
    bool started;
    TaskHandle_t task;
    int num_of_entries;
    dns_entry_pair_t entry[];
};

static char *dns_parse_name(char *raw_name, char *parsed_name, size_t parsed_name_max_len)
{
    char *label = raw_name;
    char *name_itr = parsed_name;
    size_t name_len = 0;

    do {
        size_t sub_name_len = (size_t)*label;
        name_len += sub_name_len + 1U;
        if (name_len > parsed_name_max_len) {
            return NULL;
        }

        memcpy(name_itr, label + 1, sub_name_len);
        name_itr[sub_name_len] = '.';
        name_itr += sub_name_len + 1U;
        label += sub_name_len + 1U;
    } while (*label != 0);

    parsed_name[name_len - 1U] = '\0';
    return label + 1;
}

static int dns_build_reply(char *request, size_t request_len, char *reply, size_t reply_max_len,
                           dns_server_handle_t handle)
{
    if (request_len > reply_max_len) {
        return -1;
    }

    memset(reply, 0, reply_max_len);
    memcpy(reply, request, request_len);

    dns_header_t *header = (dns_header_t *)reply;
    if ((header->flags & DNS_OPCODE_MASK) != 0) {
        return 0;
    }

    header->flags |= DNS_QR_FLAG;

    const uint16_t question_count = ntohs(header->qd_count);
    header->an_count = htons(question_count);

    const int reply_len = (int)request_len + ((int)question_count * (int)sizeof(dns_answer_t));
    if (reply_len > (int)reply_max_len) {
        return -1;
    }

    char *answer_ptr = reply + request_len;
    char *question_ptr = reply + sizeof(dns_header_t);
    char name[128];

    for (uint16_t i = 0; i < question_count; ++i) {
        char *name_end_ptr = dns_parse_name(question_ptr, name, sizeof(name));
        if (name_end_ptr == NULL) {
            ESP_LOGE(TAG, "Failed to parse DNS name");
            return -1;
        }

        dns_question_t *question = (dns_question_t *)name_end_ptr;
        const uint16_t type = ntohs(question->type);
        const uint16_t qclass = ntohs(question->class);

        if (type == DNS_A_RECORD) {
            esp_ip4_addr_t ip = { .addr = IPADDR_ANY };
            for (int entry_index = 0; entry_index < handle->num_of_entries; ++entry_index) {
                const dns_entry_pair_t *entry = &handle->entry[entry_index];
                if (strcmp(entry->name, "*") != 0 && strcmp(entry->name, name) != 0) {
                    continue;
                }

                if (entry->if_key != NULL) {
                    esp_netif_ip_info_t ip_info;
                    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey(entry->if_key), &ip_info);
                    ip.addr = ip_info.ip.addr;
                    break;
                }

                if (entry->ip.addr != IPADDR_ANY) {
                    ip.addr = entry->ip.addr;
                    break;
                }
            }

            if (ip.addr != IPADDR_ANY) {
                dns_answer_t *answer = (dns_answer_t *)answer_ptr;
                answer->ptr_offset = htons(0xC000 | (question_ptr - reply));
                answer->type = htons(type);
                answer->class = htons(qclass);
                answer->ttl = htonl(DNS_ANSWER_TTL_SEC);
                answer->addr_len = htons(sizeof(ip.addr));
                answer->ip_addr = ip.addr;
                answer_ptr += sizeof(dns_answer_t);
            }
        }

        question_ptr = name_end_ptr + sizeof(dns_question_t);
    }

    return (int)(answer_ptr - reply);
}

static void dns_server_task(void *ctx)
{
    dns_server_handle_t handle = ctx;
    char rx_buffer[128];
    char addr_str[128];

    while (handle->started) {
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_ANY),
            .sin_port = htons(DNS_PORT),
        };

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno=%d", errno);
            break;
        }

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(TAG, "Unable to bind DNS socket: errno=%d", errno);
            close(sock);
            break;
        }

        while (handle->started) {
            struct sockaddr_in6 source_addr;
            socklen_t socklen = sizeof(source_addr);
            const int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                                     (struct sockaddr *)&source_addr, &socklen);
            if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno=%d", errno);
                break;
            }

            if (source_addr.sin6_family == PF_INET) {
                inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr,
                            addr_str, sizeof(addr_str) - 1);
            } else {
                inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
            }

            rx_buffer[len] = 0;

            char reply[DNS_REPLY_BUFFER_LEN];
            const int reply_len = dns_build_reply(rx_buffer, (size_t)len, reply, sizeof(reply), handle);
            if (reply_len <= 0) {
                continue;
            }

            if (sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr)) < 0) {
                ESP_LOGE(TAG, "sendto failed for %s: errno=%d", addr_str, errno);
                break;
            }
        }

        shutdown(sock, 0);
        close(sock);
    }

    vTaskDelete(NULL);
}

dns_server_handle_t start_dns_server(dns_server_config_t *config)
{
    dns_server_handle_t handle = calloc(1, sizeof(struct dns_server_handle) +
                                           (size_t)config->num_of_entries * sizeof(dns_entry_pair_t));
    ESP_RETURN_ON_FALSE(handle != NULL, NULL, TAG, "Failed to allocate DNS server");

    handle->started = true;
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry, config->item, (size_t)config->num_of_entries * sizeof(dns_entry_pair_t));

    if (xTaskCreate(dns_server_task, "dns_server", 4096, handle, 5, &handle->task) != pdPASS) {
        free(handle);
        return NULL;
    }

    return handle;
}

void stop_dns_server(dns_server_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    handle->started = false;
    if (handle->task != NULL) {
        vTaskDelete(handle->task);
    }
    free(handle);
}
