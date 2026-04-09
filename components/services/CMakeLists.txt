idf_component_register(
    SRCS "service_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES kernel freertos esp_timer log
)
