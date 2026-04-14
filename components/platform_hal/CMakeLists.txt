idf_component_register(
    SRCS "hal_system.c" "log_control.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support log
)
