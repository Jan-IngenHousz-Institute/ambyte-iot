# Keep the release image free of the verification controls: sources compile
# only with CONFIG_AMBYTE_EVQ_HIL (env evq-hil).
set(evq_hil_srcs "")
if(CONFIG_AMBYTE_EVQ_HIL)
    list(APPEND evq_hil_srcs "evq_hil.c" "evq_hil_payload.c")
endif()

idf_component_register(
    SRCS ${evq_hil_srcs}
    INCLUDE_DIRS "include"
    REQUIRES event_log
    PRIV_REQUIRES console device_commands sync_runner sd_card app_update esp_timer mbedtls nvs_flash spi_flash esp_partition fatfs
)
