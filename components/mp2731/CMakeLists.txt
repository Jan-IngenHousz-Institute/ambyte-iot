idf_component_register(
    SRCS
        "mp2731.c"
        "mp2731_core.c"
        "mp2731_diagnostics.c"
        "mp2731_redetect.c"
        "mp2731_aico.c"
    INCLUDE_DIRS "include"
    REQUIRES domain
    PRIV_REQUIRES i2c_bus esp_timer esp_system
)
