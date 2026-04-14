#include "services/wifi_service.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "lwip/inet.h"

#include "kernel/kernel_msgbus.h"
#include "services/dns_server.h"

#define WIFI_NAMESPACE                  "wifi_portal"
#define WIFI_KEY_SSID                   "ssid"
#define WIFI_KEY_PASSWORD               "password"
#define WIFI_MAX_RETRY_COUNT            8
#define WIFI_SCAN_RESULT_LIMIT          12
#define WIFI_SCAN_JSON_BUFFER_SIZE      2048
#define WIFI_POST_BODY_LIMIT            256
#define WIFI_AP_CHANNEL                 6
#define WIFI_AP_MAX_CONNECTIONS         4
#define WIFI_AP_IP_STR_LEN              16
#define WIFI_STA_IP_STR_LEN             16
#define WIFI_PORTAL_TX_POWER_QUARTER_DBM 40
#define WIFI_PORTAL_LISTEN_INTERVAL     10

static const char *TAG = "wifi_svc";

static const char s_portal_page[] =
"<!doctype html>"
"<html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 Collar Setup</title>"
"<style>"
"body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:#f5f6f8;color:#18212f;}"
".wrap{max-width:720px;margin:0 auto;padding:24px;}"
".card{background:#fff;border-radius:18px;padding:24px;box-shadow:0 16px 40px rgba(0,0,0,.08);}"
"h1{margin:0 0 8px;font-size:28px;}p{line-height:1.5;color:#556070;}"
".row{display:flex;gap:12px;flex-wrap:wrap;}button{border:0;border-radius:12px;padding:12px 16px;background:#0c7c59;color:#fff;font-weight:700;cursor:pointer;}"
"button.alt{background:#e7edf4;color:#18212f;}input,select{width:100%;box-sizing:border-box;border:1px solid #ccd5e0;border-radius:12px;padding:12px 14px;font-size:16px;}"
"label{display:block;margin:16px 0 8px;font-weight:700;}small{color:#6b7480;}#status,#result{margin-top:14px;padding:12px 14px;border-radius:12px;background:#eef4ff;color:#22416e;}"
"ul{list-style:none;padding:0;margin:14px 0 0;}li{padding:12px;border:1px solid #e4e8ef;border-radius:12px;margin-bottom:10px;background:#fafbfd;cursor:pointer;}"
".ssid{font-weight:700;}.meta{font-size:13px;color:#66717f;margin-top:4px;}"
"</style></head><body><div class='wrap'><div class='card'>"
"<h1>设备配网</h1>"
"<p>连接到这个 ESP32 热点后，在这里选择家里的 Wi-Fi 并提交。设备连上路由器后会自动切换到正常联网模式。</p>"
"<div id='status'>正在读取设备状态...</div>"
"<div class='row' style='margin-top:14px'><button id='scanBtn' type='button'>扫描附近 Wi-Fi</button><button class='alt' id='refreshBtn' type='button'>刷新状态</button></div>"
"<ul id='networks'></ul>"
"<form id='wifiForm'>"
"<label for='ssid'>Wi-Fi 名称</label><input id='ssid' name='ssid' maxlength='32' placeholder='输入或点击上方列表'>"
"<label for='password'>Wi-Fi 密码</label><input id='password' name='password' maxlength='64' type='password' placeholder='开放网络可留空'>"
"<div class='row' style='margin-top:16px'><button type='submit'>保存并连接</button></div>"
"</form><div id='result' style='display:none'></div>"
"<p><small>如果手机没有自动弹出页面，也可以手动访问 <b>http://192.168.4.1/</b></small></p>"
"</div></div>"
"<script>"
"const statusEl=document.getElementById('status');"
"const resultEl=document.getElementById('result');"
"const listEl=document.getElementById('networks');"
"const ssidEl=document.getElementById('ssid');"
"const form=document.getElementById('wifiForm');"
"const API_BASE='http://192.168.4.1';"
"function showResult(msg,ok){resultEl.style.display='block';resultEl.style.background=ok?'#e8fff5':'#fff1f0';resultEl.style.color=ok?'#125842':'#8a2d25';resultEl.textContent=msg;}"
"function authLabel(a){return a==='open'?'开放网络':'加密网络';}"
"async function refreshStatus(){try{const res=await fetch(API_BASE+'/api/status',{cache:'no-store'});const data=await res.json();let text='配网热点: '+data.ap_ssid+' ('+data.ap_ip+')';if(data.state){text+=' | 状态: '+data.state;}if(data.sta_ssid){text+=' | 当前目标: '+data.sta_ssid;}if(data.sta_ip){text+=' | 设备IP: '+data.sta_ip;}statusEl.textContent=text;}catch(e){statusEl.textContent='读取状态失败，请重试';}}"
"async function scan(){listEl.innerHTML='<li>正在扫描，请稍候...</li>';try{const res=await fetch(API_BASE+'/api/scan',{cache:'no-store'});const data=await res.json();if(!data.networks||!data.networks.length){listEl.innerHTML='<li>没有扫描到 2.4G Wi-Fi，请靠近路由器后再试</li>';return;}listEl.innerHTML='';data.networks.forEach(net=>{const li=document.createElement('li');li.innerHTML='<div class=\"ssid\">'+net.ssid+'</div><div class=\"meta\">'+authLabel(net.auth)+' | RSSI '+net.rssi+' dBm</div>';li.onclick=()=>{ssidEl.value=net.ssid;window.scrollTo({top:document.body.scrollHeight,behavior:'smooth'});};listEl.appendChild(li);});}catch(e){listEl.innerHTML='<li>扫描失败，请稍后再试</li>';}}"
"document.getElementById('scanBtn').onclick=scan;"
"document.getElementById('refreshBtn').onclick=refreshStatus;"
"form.onsubmit=async(ev)=>{ev.preventDefault();const body=new URLSearchParams(new FormData(form)).toString();showResult('正在保存并尝试连接...',true);try{const res=await fetch(API_BASE+'/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});const data=await res.json();showResult(data.message||'已提交',res.ok);refreshStatus();}catch(e){showResult('提交失败，请重试',false);}};"
"refreshStatus();scan();setInterval(refreshStatus,3000);"
"</script></body></html>";

typedef struct {
    bool initialized;
    bool wifi_started;
    bool portal_active;
    bool sta_connected;
    bool sta_got_ip;
    bool has_credentials;
    uint8_t retry_count;
    httpd_handle_t http_server;
    dns_server_handle_t dns_server;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    char ap_ssid[33];
    char ap_ip[WIFI_AP_IP_STR_LEN];
    char sta_ssid[33];
    char sta_password[65];
    char sta_ip[WIFI_STA_IP_STR_LEN];
    char last_error[96];
} wifi_service_state_t;

static wifi_service_state_t s_wifi;

static esp_err_t wifi_service_publish_state(kernel_wifi_state_t state)
{
    const kernel_msg_t msg = {
        .topic = KERNEL_TOPIC_WIFI_STATE,
        .source = KERNEL_SOURCE_SERVICE,
        .value = (uint32_t)state,
        .timestamp_us = esp_timer_get_time(),
    };

    return kernel_msgbus_publish(&msg, pdMS_TO_TICKS(10));
}

static void wifi_service_set_error(const char *error)
{
    strlcpy(s_wifi.last_error, error != NULL ? error : "", sizeof(s_wifi.last_error));
}

static void wifi_service_format_ip(esp_ip4_addr_t ip, char *out, size_t out_len)
{
    if (out_len == 0U) {
        return;
    }
    inet_ntoa_r(ip.addr, out, out_len);
}

static const char *wifi_service_auth_to_string(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa_wpa2";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2_wpa3";
    default:
        return "unknown";
    }
}

static const char *wifi_service_state_string(void)
{
    if (s_wifi.portal_active && !s_wifi.sta_got_ip) {
        return "provisioning";
    }
    if (s_wifi.sta_got_ip) {
        return "got_ip";
    }
    if (s_wifi.sta_connected) {
        return "connected";
    }
    if (s_wifi.has_credentials) {
        return "connecting";
    }
    return "idle";
}

static bool wifi_service_is_started(void)
{
    return s_wifi.wifi_started;
}

static esp_err_t wifi_service_load_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        s_wifi.has_credentials = false;
        s_wifi.sta_ssid[0] = '\0';
        s_wifi.sta_password[0] = '\0';
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    size_t ssid_len = sizeof(s_wifi.sta_ssid);
    ret = nvs_get_str(handle, WIFI_KEY_SSID, s_wifi.sta_ssid, &ssid_len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return (ret == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : ret;
    }

    size_t password_len = sizeof(s_wifi.sta_password);
    ret = nvs_get_str(handle, WIFI_KEY_PASSWORD, s_wifi.sta_password, &password_len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        s_wifi.sta_password[0] = '\0';
        ret = ESP_OK;
    }

    nvs_close(handle);
    if (ret == ESP_OK) {
        s_wifi.has_credentials = s_wifi.sta_ssid[0] != '\0';
    }
    return ret;
}

static esp_err_t wifi_service_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(handle, WIFI_KEY_SSID, ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, WIFI_KEY_PASSWORD, password);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        strlcpy(s_wifi.sta_ssid, ssid, sizeof(s_wifi.sta_ssid));
        strlcpy(s_wifi.sta_password, password, sizeof(s_wifi.sta_password));
        s_wifi.has_credentials = true;
    }

    return ret;
}

static esp_err_t wifi_service_start_webserver(void);
static void wifi_service_stop_webserver(void);
static esp_err_t wifi_service_start_portal(void);
static void wifi_service_stop_portal(void);
static esp_err_t wifi_service_connect_sta(void);
static esp_err_t wifi_service_scan_networks(wifi_ap_record_t *records, uint16_t *ap_count);

static void wifi_service_apply_low_power_profile(wifi_mode_t mode)
{
    esp_err_t ret;

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        ret = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set AP bandwidth HT20: %s", esp_err_to_name(ret));
        }
    }

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        ret = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set STA power save: %s", esp_err_to_name(ret));
        }

        ret = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set STA bandwidth HT20: %s", esp_err_to_name(ret));
        }
    }

    ret = esp_wifi_set_max_tx_power(WIFI_PORTAL_TX_POWER_QUARTER_DBM);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reduce Wi-Fi TX power: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Wi-Fi TX power limited to %.2f dBm for provisioning",
                 WIFI_PORTAL_TX_POWER_QUARTER_DBM / 4.0f);
    }
}

static void wifi_service_update_ap_identity(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_get_mac(WIFI_IF_STA, mac));
    snprintf(s_wifi.ap_ssid, sizeof(s_wifi.ap_ssid), "Collar-Setup-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

static esp_err_t wifi_service_start_wifi_if_needed(void)
{
    if (s_wifi.wifi_started) {
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_start();
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_wifi.wifi_started = true;
        return ESP_OK;
    }
    return ret;
}

static esp_err_t wifi_service_configure_ap(void)
{
    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, s_wifi.ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_wifi.ap_ssid);
    ap_config.ap.channel = WIFI_AP_CHANNEL;
    ap_config.ap.max_connection = WIFI_AP_MAX_CONNECTIONS;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.beacon_interval = 500;
    ap_config.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "Failed to switch to AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config), TAG, "Failed to set AP config");
    ESP_RETURN_ON_ERROR(wifi_service_start_wifi_if_needed(), TAG, "Failed to start Wi-Fi");
    wifi_service_apply_low_power_profile(WIFI_MODE_AP);

    esp_netif_ip_info_t ip_info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_wifi.ap_netif, &ip_info), TAG, "Failed to get AP IP");
    wifi_service_format_ip(ip_info.ip, s_wifi.ap_ip, sizeof(s_wifi.ap_ip));

    char captive_url[32];
    snprintf(captive_url, sizeof(captive_url), "http://%s/", s_wifi.ap_ip);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_wifi.ap_netif));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
        s_wifi.ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, captive_url, strlen(captive_url)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_wifi.ap_netif));

    ESP_LOGI(TAG, "SoftAP portal active: ssid=%s ip=%s", s_wifi.ap_ssid, s_wifi.ap_ip);
    return ESP_OK;
}

static esp_err_t wifi_service_start_portal(void)
{
    if (s_wifi.portal_active) {
        return ESP_OK;
    }

    esp_err_t ret = wifi_service_configure_ap();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = wifi_service_start_webserver();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_wifi.dns_server == NULL) {
        dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_wifi.dns_server = start_dns_server(&dns_config);
        if (s_wifi.dns_server == NULL) {
            return ESP_FAIL;
        }
    }

    s_wifi.portal_active = true;
    wifi_service_set_error("");
    (void)wifi_service_publish_state(KERNEL_WIFI_STATE_PROVISIONING);
    return ESP_OK;
}

static void wifi_service_stop_portal(void)
{
    if (s_wifi.dns_server != NULL) {
        stop_dns_server(s_wifi.dns_server);
        s_wifi.dns_server = NULL;
    }

    wifi_service_stop_webserver();
    s_wifi.portal_active = false;
}

static esp_err_t wifi_service_connect_sta(void)
{
    if (!s_wifi.has_credentials || s_wifi.sta_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, s_wifi.sta_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, s_wifi.sta_password, sizeof(sta_config.sta.password));
    sta_config.sta.failure_retry_cnt = 0;
    sta_config.sta.listen_interval = WIFI_PORTAL_LISTEN_INTERVAL;
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    if (s_wifi.portal_active) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Failed to keep APSTA mode");
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to switch to STA mode");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config), TAG, "Failed to set STA config");
    ESP_RETURN_ON_ERROR(wifi_service_start_wifi_if_needed(), TAG, "Failed to start Wi-Fi");
    wifi_service_apply_low_power_profile(s_wifi.portal_active ? WIFI_MODE_APSTA : WIFI_MODE_STA);

    s_wifi.retry_count = 0;
    s_wifi.sta_connected = false;
    s_wifi.sta_got_ip = false;
    s_wifi.sta_ip[0] = '\0';
    wifi_service_set_error("");

    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "Failed to begin STA connect");

    ESP_LOGI(TAG, "Connecting to Wi-Fi ssid=%s", s_wifi.sta_ssid);
    (void)wifi_service_publish_state(KERNEL_WIFI_STATE_CONNECTING);
    return ESP_OK;
}

static size_t wifi_service_json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t written = 0;

    if (dst_len == 0U) {
        return 0;
    }

    while (src != NULL && *src != '\0' && written + 2U < dst_len) {
        if (*src == '\\' || *src == '"') {
            if (written + 3U >= dst_len) {
                break;
            }
            dst[written++] = '\\';
        }
        dst[written++] = *src++;
    }
    dst[written] = '\0';
    return written;
}

static void wifi_service_url_decode(char *text)
{
    char *read_ptr = text;
    char *write_ptr = text;

    while (*read_ptr != '\0') {
        if (*read_ptr == '+' ) {
            *write_ptr++ = ' ';
            read_ptr++;
            continue;
        }

        if (*read_ptr == '%' && read_ptr[1] != '\0' && read_ptr[2] != '\0') {
            char hex[3] = { read_ptr[1], read_ptr[2], '\0' };
            *write_ptr++ = (char)strtol(hex, NULL, 16);
            read_ptr += 3;
            continue;
        }

        *write_ptr++ = *read_ptr++;
    }

    *write_ptr = '\0';
}

static esp_err_t wifi_service_root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_portal_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_service_status_get_handler(httpd_req_t *req)
{
    char escaped_ap_ssid[80];
    char escaped_sta_ssid[80];
    char escaped_error[160];

    wifi_service_json_escape(escaped_ap_ssid, sizeof(escaped_ap_ssid), s_wifi.ap_ssid);
    wifi_service_json_escape(escaped_sta_ssid, sizeof(escaped_sta_ssid), s_wifi.sta_ssid);
    wifi_service_json_escape(escaped_error, sizeof(escaped_error), s_wifi.last_error);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"state\":\"");
    httpd_resp_sendstr_chunk(req, wifi_service_state_string());
    httpd_resp_sendstr_chunk(req, "\",\"portal_active\":");
    httpd_resp_sendstr_chunk(req, s_wifi.portal_active ? "true" : "false");
    httpd_resp_sendstr_chunk(req, ",\"ap_ssid\":\"");
    httpd_resp_sendstr_chunk(req, escaped_ap_ssid);
    httpd_resp_sendstr_chunk(req, "\",\"ap_ip\":\"");
    httpd_resp_sendstr_chunk(req, s_wifi.ap_ip);
    httpd_resp_sendstr_chunk(req, "\",\"sta_ssid\":\"");
    httpd_resp_sendstr_chunk(req, escaped_sta_ssid);
    httpd_resp_sendstr_chunk(req, "\",\"sta_ip\":\"");
    httpd_resp_sendstr_chunk(req, s_wifi.sta_ip);
    httpd_resp_sendstr_chunk(req, "\",\"last_error\":\"");
    httpd_resp_sendstr_chunk(req, escaped_error);
    httpd_resp_sendstr_chunk(req, "\"}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t wifi_service_scan_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t records[WIFI_SCAN_RESULT_LIMIT];
    uint16_t ap_count = WIFI_SCAN_RESULT_LIMIT;
    char json[WIFI_SCAN_JSON_BUFFER_SIZE];
    size_t used = 0;

    if (!wifi_service_is_started()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi not started");
        return ESP_FAIL;
    }

    esp_err_t ret = wifi_service_scan_networks(records, &ap_count);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ret;
    }

    used += (size_t)snprintf(json + used, sizeof(json) - used, "{\"networks\":[");
    for (uint16_t i = 0; i < ap_count; ++i) {
        char escaped_ssid[80];
        if (records[i].ssid[0] == '\0') {
            continue;
        }

        wifi_service_json_escape(escaped_ssid, sizeof(escaped_ssid), (const char *)records[i].ssid);
        used += (size_t)snprintf(json + used, sizeof(json) - used,
                                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
                                 (used > 13U) ? "," : "",
                                 escaped_ssid,
                                 records[i].rssi,
                                 wifi_service_auth_to_string(records[i].authmode));
        if (used >= sizeof(json)) {
            break;
        }
    }
    (void)snprintf(json + MIN(used, sizeof(json) - 1U), sizeof(json) - MIN(used, sizeof(json) - 1U), "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_service_config_post_handler(httpd_req_t *req)
{
    char body[WIFI_POST_BODY_LIMIT + 1];
    char ssid_raw[WIFI_POST_BODY_LIMIT + 1] = {0};
    char password_raw[WIFI_POST_BODY_LIMIT + 1] = {0};
    int received = 0;

    if (req->content_len <= 0 || req->content_len > WIFI_POST_BODY_LIMIT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid payload");
        return ESP_FAIL;
    }

    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    if (httpd_query_key_value(body, WIFI_KEY_SSID, ssid_raw, sizeof(ssid_raw)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid missing");
        return ESP_FAIL;
    }
    (void)httpd_query_key_value(body, WIFI_KEY_PASSWORD, password_raw, sizeof(password_raw));

    wifi_service_url_decode(ssid_raw);
    wifi_service_url_decode(password_raw);

    if (ssid_raw[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid empty");
        return ESP_FAIL;
    }

    const size_t ssid_len = strlen(ssid_raw);
    const size_t password_len = strlen(password_raw);
    if (ssid_len > 32U) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"message\":\"Wi-Fi 名称过长，请控制在 32 字节内\"}");
        return ESP_ERR_INVALID_SIZE;
    }
    if (password_len > 64U) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"message\":\"Wi-Fi 密码过长，请控制在 64 字节内\"}");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = wifi_service_save_credentials(ssid_raw, password_raw);
    if (ret == ESP_OK) {
        ret = wifi_service_connect_sta();
    }

    httpd_resp_set_type(req, "application/json");
    if (ret != ESP_OK) {
        wifi_service_set_error("save/connect failed");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"message\":\"保存失败，请稍后重试\"}");
        return ret;
    }

    httpd_resp_sendstr(req, "{\"message\":\"凭据已保存，设备正在尝试联网\"}");
    return ESP_OK;
}

static esp_err_t wifi_service_redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_service_probe_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_service_scan_networks(wifi_ap_record_t *records, uint16_t *ap_count)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };

    ESP_RETURN_ON_FALSE(records != NULL && ap_count != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid scan buffer");
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "Failed to get current Wi-Fi mode");

    bool restore_ap_only = false;
    if (mode == WIFI_MODE_AP) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Failed to enable STA for scan");
        wifi_service_apply_low_power_profile(WIFI_MODE_APSTA);
        restore_ap_only = true;
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret == ESP_OK) {
        ret = esp_wifi_scan_get_ap_records(ap_count, records);
    }

    if (restore_ap_only) {
        esp_err_t restore_ret = esp_wifi_set_mode(WIFI_MODE_AP);
        if (restore_ret == ESP_OK) {
            wifi_service_apply_low_power_profile(WIFI_MODE_AP);
        } else {
            ESP_LOGW(TAG, "Failed to restore AP-only mode after scan: %s", esp_err_to_name(restore_ret));
        }
    }

    return ret;
}

static esp_err_t wifi_service_start_webserver(void)
{
    if (s_wifi.http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // ESP-IDF reserves a few sockets internally for the HTTP server, so keep this
    // comfortably below the lwIP limit used by the current project configuration.
    config.max_open_sockets = 4;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_wifi.http_server, &config), TAG, "Failed to start HTTP server");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = wifi_service_root_get_handler,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = wifi_service_status_get_handler,
    };
    const httpd_uri_t scan = {
        .uri = "/api/scan",
        .method = HTTP_GET,
        .handler = wifi_service_scan_get_handler,
    };
    const httpd_uri_t config_post = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = wifi_service_config_post_handler,
    };
    const httpd_uri_t generate_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = wifi_service_probe_handler,
    };
    const httpd_uri_t gen_204 = {
        .uri = "/gen_204",
        .method = HTTP_GET,
        .handler = wifi_service_probe_handler,
    };
    const httpd_uri_t hotspot_detect = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = wifi_service_probe_handler,
    };
    const httpd_uri_t ncsi = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = wifi_service_probe_handler,
    };
    const httpd_uri_t connecttest = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = wifi_service_probe_handler,
    };

    httpd_register_uri_handler(s_wifi.http_server, &root);
    httpd_register_uri_handler(s_wifi.http_server, &status);
    httpd_register_uri_handler(s_wifi.http_server, &scan);
    httpd_register_uri_handler(s_wifi.http_server, &config_post);
    httpd_register_uri_handler(s_wifi.http_server, &generate_204);
    httpd_register_uri_handler(s_wifi.http_server, &gen_204);
    httpd_register_uri_handler(s_wifi.http_server, &hotspot_detect);
    httpd_register_uri_handler(s_wifi.http_server, &ncsi);
    httpd_register_uri_handler(s_wifi.http_server, &connecttest);
    httpd_register_err_handler(s_wifi.http_server, HTTPD_404_NOT_FOUND, wifi_service_redirect_handler);
    return ESP_OK;
}

static void wifi_service_stop_webserver(void)
{
    if (s_wifi.http_server != NULL) {
        httpd_stop(s_wifi.http_server);
        s_wifi.http_server = NULL;
    }
}

static void wifi_service_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                            int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *event = event_data;
            ESP_LOGI(TAG, "Portal client joined: " MACSTR " aid=%d", MAC2STR(event->mac), event->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *event = event_data;
            ESP_LOGI(TAG, "Portal client left: " MACSTR " aid=%d reason=%d",
                     MAC2STR(event->mac), event->aid, event->reason);
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
            s_wifi.sta_connected = true;
            ESP_LOGI(TAG, "STA connected to ssid=%s", s_wifi.sta_ssid);
            (void)wifi_service_publish_state(KERNEL_WIFI_STATE_CONNECTED);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *event = event_data;
            s_wifi.sta_connected = false;
            s_wifi.sta_got_ip = false;
            s_wifi.sta_ip[0] = '\0';
            ESP_LOGW(TAG, "STA disconnected: reason=%d retry=%u/%u",
                     event->reason, s_wifi.retry_count, WIFI_MAX_RETRY_COUNT);
            (void)wifi_service_publish_state(KERNEL_WIFI_STATE_DISCONNECTED);

            if (!s_wifi.has_credentials) {
                break;
            }

            if (s_wifi.retry_count < WIFI_MAX_RETRY_COUNT) {
                s_wifi.retry_count++;
                esp_wifi_connect();
                (void)wifi_service_publish_state(KERNEL_WIFI_STATE_CONNECTING);
            } else {
                wifi_service_set_error("连接路由器失败，已回退到配网页面");
                (void)wifi_service_publish_state(KERNEL_WIFI_STATE_FAILED);
                if (wifi_service_start_portal() != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to restart SoftAP portal after STA failure");
                }
            }
            break;
        }
        default:
            break;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_wifi.retry_count = 0;
        s_wifi.sta_connected = true;
        s_wifi.sta_got_ip = true;
        wifi_service_format_ip(event->ip_info.ip, s_wifi.sta_ip, sizeof(s_wifi.sta_ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_wifi.sta_ip);
        wifi_service_set_error("");
        (void)wifi_service_publish_state(KERNEL_WIFI_STATE_GOT_IP);

        if (s_wifi.portal_active) {
            wifi_service_stop_portal();
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_LOGI(TAG, "Provisioning complete, SoftAP portal stopped");
        }
    }
}

esp_err_t wifi_service_start(void)
{
    if (s_wifi.initialized) {
        return ESP_OK;
    }

    memset(&s_wifi, 0, sizeof(s_wifi));
    (void)wifi_service_publish_state(KERNEL_WIFI_STATE_STARTING);

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    s_wifi.ap_netif = esp_netif_create_default_wifi_ap();
    if (s_wifi.sta_netif == NULL || s_wifi.ap_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Failed to init Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Failed to set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_service_wifi_event_handler, NULL),
        TAG, "Failed to register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_service_wifi_event_handler, NULL),
        TAG, "Failed to register IP event handler");

    wifi_service_update_ap_identity();
    snprintf(s_wifi.ap_ip, sizeof(s_wifi.ap_ip), "%s", "192.168.4.1");
    s_wifi.initialized = true;

    ret = wifi_service_load_credentials();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_wifi.has_credentials) {
        ESP_LOGI(TAG, "Stored Wi-Fi credentials found, trying STA connect to %s", s_wifi.sta_ssid);
        ret = wifi_service_connect_sta();
    } else {
        ESP_LOGI(TAG, "No stored Wi-Fi credentials, starting SoftAP portal");
        ret = wifi_service_start_portal();
    }

    if (ret != ESP_OK) {
        (void)wifi_service_publish_state(KERNEL_WIFI_STATE_FAILED);
        return ret;
    }

    return ESP_OK;
}
