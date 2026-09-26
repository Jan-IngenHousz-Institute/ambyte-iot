set(event_log_srcs "event_log.c" "evlog_inventory.c" "evq_index.c" "evq_render.c")
# On-device verification build only (CONFIG_AMBYTE_EVQ_HIL, env evq-hil): the
# I/O trace/fault wrapper. The release image never compiles these files.
if(CONFIG_AMBYTE_EVQ_HIL)
    list(APPEND event_log_srcs "evq_hil_io.c" "evq_hil_trace.c")
endif()

idf_component_register(
    SRCS ${event_log_srcs}
    INCLUDE_DIRS "include"
    # sd_card is the SD overflow medium (spool primary + mirror, archive, legacy
    # import), always bracketed by sdcard_io_begin/end; the store itself is
    # littlefs on the internal "storage" partition and never waits on the card.
    REQUIRES domain sd_card nvs_flash littlefs
    PRIV_REQUIRES esp_timer mbedtls
)
