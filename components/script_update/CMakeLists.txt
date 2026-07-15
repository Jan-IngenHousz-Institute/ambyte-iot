idf_component_register(
    SRCS "script_update.c"
    INCLUDE_DIRS "include"
    REQUIRES domain lua_runner
    PRIV_REQUIRES nvs_flash mbedtls sd_card lua esp_http_client esp_system
)
