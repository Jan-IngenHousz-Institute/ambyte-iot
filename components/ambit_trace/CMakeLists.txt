idf_component_register(
    SRCS "ambit_trace.c"
    INCLUDE_DIRS "include"
    REQUIRES device_commands event_log payload_codec uart_sensors domain
)
