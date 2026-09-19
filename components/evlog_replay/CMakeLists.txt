idf_component_register(
    SRCS "evlog_replay.c"
    INCLUDE_DIRS "include"
    REQUIRES domain event_log sd_card nvs_flash esp_timer
)
