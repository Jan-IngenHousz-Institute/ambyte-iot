idf_component_register(
    SRCS "status_heartbeat.c"
    INCLUDE_DIRS "include"
    REQUIRES domain
    PRIV_REQUIRES json esp_timer fleet_jitter
)
