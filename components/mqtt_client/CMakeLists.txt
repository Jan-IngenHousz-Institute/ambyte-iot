idf_component_register(
    SRCS "mqtt_client.c"
    INCLUDE_DIRS "include"
    REQUIRES domain
    PRIV_REQUIRES mqtt esp_event esp_timer
)
