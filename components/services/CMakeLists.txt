idf_component_register(
    SRCS "service_manager.c" "wifi_service.c" "wifi_control_service.c" "cloud_service.c" "conversation_service.c" "dns_server.c"
    INCLUDE_DIRS "include"
    REQUIRES kernel freertos esp_timer log esp_event esp_netif esp_wifi lwip esp_http_server nvs_flash
)
