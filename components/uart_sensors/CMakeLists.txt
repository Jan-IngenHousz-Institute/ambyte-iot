idf_component_register(
    SRCS "uart_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES domain uart_stream
    PRIV_REQUIRES driver esp_timer
)
