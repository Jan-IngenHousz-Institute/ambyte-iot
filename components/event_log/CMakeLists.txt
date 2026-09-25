idf_component_register(
    SRCS "event_log.c" "evlog_inventory.c" "evq_index.c" "evq_render.c"
    INCLUDE_DIRS "include"
    # sd_card is the SD overflow medium (spool primary + mirror, archive, legacy
    # import), always bracketed by sdcard_io_begin/end; the store itself is
    # littlefs on the internal "storage" partition and never waits on the card.
    REQUIRES domain sd_card nvs_flash littlefs
)
