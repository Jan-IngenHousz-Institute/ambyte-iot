idf_component_register(
    SRCS "sd_logger.c"
    INCLUDE_DIRS "."
    REQUIRES sd_card
    # event_log: only for evq_hil_io_w.h — the verification build's writer-tagged
    # fault wrappers (empty header in a release build).
    PRIV_REQUIRES event_log
)
# Cold SD-fault/diagnostic paths: compiled for size, not the project's -Og
# (keeps the write-integrity release inside its flash budget).
set_source_files_properties("sd_logger.c" PROPERTIES COMPILE_OPTIONS "-Os")
