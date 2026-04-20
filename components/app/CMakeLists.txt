idf_component_register(
    SRCS "app_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES kernel freertos esp_timer log services bsp lwip
)
