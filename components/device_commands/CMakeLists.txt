idf_component_register(
    SRCS "device_commands.c"
    INCLUDE_DIRS "include"
    REQUIRES domain event_log
    PRIV_REQUIRES json esp_wifi esp_timer esp_driver_ledc wifi_manager esp_pm timezone clock_trust esp_app_format
)
