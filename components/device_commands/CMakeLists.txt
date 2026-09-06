idf_component_register(
    SRCS "device_commands.c" "envelope_provenance.c"
    INCLUDE_DIRS "include"
    REQUIRES domain event_log payload_codec ambit_announcement
    PRIV_REQUIRES json esp_wifi esp_timer esp_driver_ledc wifi_manager esp_pm timezone clock_trust esp_app_format
)
