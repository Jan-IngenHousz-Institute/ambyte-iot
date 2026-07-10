idf_component_register(
    SRCS "ambit_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES domain device_commands
    PRIV_REQUIRES esp_http_client mbedtls esp_system sd_card nvs_flash ambit_flash
)
