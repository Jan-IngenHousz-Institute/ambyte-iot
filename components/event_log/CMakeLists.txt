idf_component_register(
    SRCS "event_log.c"
    INCLUDE_DIRS "include"
    # sd_card remains a dependency ONLY for the bulk-archive/legacy-import SD
    # interchange (bracketed sdcard_io_begin/end); the store itself is littlefs
    # on the internal "storage" partition.
    REQUIRES domain sd_card nvs_flash littlefs
)
