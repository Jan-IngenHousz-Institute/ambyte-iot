idf_component_register(
    SRCS
        "mp2731.c"
        "mp2731_core.c"
    INCLUDE_DIRS "include"
    REQUIRES domain
    PRIV_REQUIRES i2c_bus esp_timer
)
