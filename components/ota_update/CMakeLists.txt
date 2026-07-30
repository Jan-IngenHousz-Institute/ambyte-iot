idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "include"
    REQUIRES domain
    PRIV_REQUIRES app_update esp_https_ota esp_http_client mbedtls nvs_flash esp_system esp_timer fleet_jitter
)
