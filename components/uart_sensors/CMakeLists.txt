idf_component_register(
    SRCS "uart_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES domain uart_stream sensor_inventory
    PRIV_REQUIRES driver esp_timer
)
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
