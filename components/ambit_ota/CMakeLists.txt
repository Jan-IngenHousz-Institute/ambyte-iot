idf_component_register(
    SRCS "ambit_ota.c" "ambit_stage.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES domain device_commands
    # event_log: only for evq_hil_io_w.h (verification-build fault wrappers).
    PRIV_REQUIRES esp_http_client mbedtls esp_system sd_card nvs_flash ambit_flash fleet_jitter event_log
)
# Cold SD-fault/diagnostic paths: compiled for size, not the project's -Og
# (keeps the write-integrity release inside its flash budget).
set_source_files_properties("ambit_stage.c" PROPERTIES COMPILE_OPTIONS "-Os")
