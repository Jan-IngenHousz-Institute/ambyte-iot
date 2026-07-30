idf_component_register(
    SRCS "sync_runner.c"
    INCLUDE_DIRS "include"
    REQUIRES device_commands clock_trust fleet_jitter
    PRIV_REQUIRES ota_update timezone nvs_flash esp_timer
)
