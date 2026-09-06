idf_component_register(
    SRCS "script_update.c"
    INCLUDE_DIRS "include"
    REQUIRES domain sched_runner sched_spec
    PRIV_REQUIRES nvs_flash mbedtls esp_http_client esp_system fleet_jitter esp_app_format
)
