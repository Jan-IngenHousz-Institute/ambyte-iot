idf_component_register(
    SRCS "diag_boot.c" "diag_crash.c" "diag_fmt.c" "diag_json.c" "diag_log.c" "diag_log_hook.c" "diag_reset.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system
    PRIV_REQUIRES espcoredump esp_partition esp_rom esp_timer log nvs_flash
)
# Cold boot-time / once-a-minute paths: compiled for size like the other
# diagnostic components (sd_diag, ambit_flash_preflight). diag_log_hook.c and
# diag_fmt.c stay at the project optimisation level: they run on every error
# line, on whatever task logged it.
set_source_files_properties("diag_boot.c" "diag_crash.c" PROPERTIES COMPILE_OPTIONS "-Os")
