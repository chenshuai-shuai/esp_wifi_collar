idf_component_register(
    SRCS
        "kernel.c"
        "kernel_msgbus.c"
        "kernel_supervisor.c"
        "kernel_trace.c"
        "kernel_workqueue.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer esp_system log
)
