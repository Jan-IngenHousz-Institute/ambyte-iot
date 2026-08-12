idf_component_register(
    SRCS "payload_v3.c" "payload_gzip.c" "uart_stream_support.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)
