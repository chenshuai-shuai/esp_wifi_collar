idf_component_register(
    SRCS "app_manager.c"
         "dialog/dialog_connection.c"
         "dialog/dialog_session.c"
         "dialog/dialog_uplink.c"
         "dialog/dialog_downlink.c"
         "dialog/dialog_playback.c"
         "dialog/dialog_orchestrator.c"
    INCLUDE_DIRS "include"
    REQUIRES kernel freertos esp_timer log services bsp lwip audio_dsp conversation driver vfs

)
