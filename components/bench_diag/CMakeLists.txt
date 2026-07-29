# Keep the production image free of the task and its diagnostic dependencies.
set(bench_diag_srcs "")
if(CONFIG_AMBYTE_BENCH_DIAG)
    list(APPEND bench_diag_srcs "bench_diag.c")
endif()

idf_component_register(
    SRCS ${bench_diag_srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi lwip
)
