idf_component_register(
    SRCS "fleet_jitter.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_hw_support
)
