idf_component_register(
    SRCS "sd_card.c" "sd_diag.c" "sd_diag_core.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver fatfs sdmmc nvs_flash esp_timer
)
# Cold SD-fault/diagnostic paths: compiled for size, not the project's -Og
# (keeps the write-integrity release inside its flash budget).
set_source_files_properties("sd_diag.c" "sd_diag_core.c" PROPERTIES COMPILE_OPTIONS "-Os")
