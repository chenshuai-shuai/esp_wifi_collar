idf_component_register(
    SRCS "conversation_client_manual.c"
    INCLUDE_DIRS "include"
    REQUIRES kernel services proto_gen esp_event mbedtls lwip freertos log esp_timer
             livekit__nanopb espressif__nghttp
    PRIV_REQUIRES nvs_flash
)
