idf_component_register(
    SRCS "mqtt_client.c"
    INCLUDE_DIRS "include"
    REQUIRES domain certs
    PRIV_REQUIRES mqtt esp_event
)
