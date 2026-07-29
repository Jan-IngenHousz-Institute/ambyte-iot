idf_component_register(
    SRCS "sync_runner.c"
    INCLUDE_DIRS "include"
    REQUIRES device_commands
    PRIV_REQUIRES ota_update timezone nvs_flash esp_timer esp_hw_support
)
