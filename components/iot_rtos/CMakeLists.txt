idf_component_register(
    SRCS
        "src/iot_rtos.c"
        "src/iot_tasks.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        iot_core
        iot_ble
    PRIV_REQUIRES
        esp_timer
        iot_drivers
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)