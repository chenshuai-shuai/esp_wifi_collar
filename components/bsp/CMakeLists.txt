idf_component_register(
    SRCS "bsp_board.c" "speaker_output.c" "microphone_input.c"
    INCLUDE_DIRS "include"
    REQUIRES platform_hal log esp_driver_i2s esp_driver_gpio esp_driver_spi esp_timer freertos
)
