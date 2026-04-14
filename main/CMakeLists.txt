idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bsp kernel services app
    PRIV_REQUIRES esp_app_format
)
