idf_component_register(
    SRCS "command_router.c"
    INCLUDE_DIRS "include"
    REQUIRES domain json nvs_flash esp_timer ota_update ambit_ota script_update
)
